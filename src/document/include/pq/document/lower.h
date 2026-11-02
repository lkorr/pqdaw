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
// Polyphony (T4.12-fix1): an instrument's setting named `voices` (Operator's) is its track's polyphony,
// ProjectTrack::maxVoices = round(voices) clamped to [1, kMaxPolyphony] (the user's value, else the patch's default);
// an instrument without one gets the link's default (LinkConfig::maxVoices). A change of it is a structural edit of
// that track whose unit restarts with a VoiceEngine of the new size (engine::ProjectCompiler::update / rebuild).
// User devices (T4.4-fix1, model.h UserPatch): the call names the device's own root, resolved through
// LowerOptions::devices (devices.h DeviceLibrary::resolve: renamed `<name>__u<id>_<graph hash>`, the NodeIds the circuit
// editor shows); the call's NodeId is the same, so a fork, an edit and Revert to Stock keep every state the patch
// still has. graph.library is then LoweredProject::userLibrary.
//
// ---- Racks (T4.8, SPEC §8.2; model.h "Racks"; QUESTIONS Q?-T4.8-a) ------------------------------------------------
// A rack lowers INLINE into its track's patch (the compiler flattens subcircuits anyway, so this is the same machine
// code as a nested call, and every device inside keeps nodeIdFor(Device, its id, 0): its state survives moves between
// chains, in and out of racks, within the track). rackSubcircuit() builds the same nodes as a patch of their own for the
// circuit view. Chains that play: active, soloed if any chain of the rack is (muted chains have no node: zero cost);
// an Instrument Rack's chains with an instrument first. Per chain c (nodes NodeRole::Chain, the chain's id):
//   Audio Effect Rack   x → [gain(x, zs_c) when its select zone is on] → its devices → gain(·, pack(gl_c, gr_c)) →
//                       summed over the chains (no chain at all: x passes; every chain muted: silence). The select
//                       zone gates the chain's INPUT (exact silence after its fade; whether it then sleeps is the
//                       scheduler's segmenting: not yet, Q?-T4.8-a).
//   Instrument Rack     gain(its instrument's voices, zk_c (key zone: Hz unit on ln voice.noteHz, Degree unit on
//                       voice.noteDegree (NaN → 0): the note's pitch / degree in its own tuning, held from the note-on,
//                       T4.17 + M4 review M7) · zv_c (velocity zone) [· the enclosing rack's per-voice gain, nested],
//                       start 1) (per voice, block rate: a zone edit while notes sound fades over the smoothing time; a
//                       note starts at its gain) → voice_sum → its effects (per-voice chains: the
//                       effects before voice_sum) → gain(·, pack(gl_c · zs_c, gr_c · zs_c)) → summed; `done` = and of
//                       every chain's `done` (all of them must have one, else none).
//   gl_c, gr_c          runtime params c<chain>_gl / _gr ("chain<id>:gl"): balance pan · volume, derived by the
//                       document (a chain mixer drag is two param writes; the `gain` stage smooths it). T4.18: while
//                       the chain's volume or pan has an automation lane (on the rack's track), the chain lowers
//                       c<chain>_volume (dB, "chain<id>:volume") and c<chain>_pan ("chain<id>:pan") instead and the
//                       same law in the graph (−∞ at kMinVolumeDb, db_to_gain, balance min(1, 1 ∓ pan)), at block rate:
//                       each lane drives one param.
//   zone gain           clip(min(v·a1 + b1, v·a2 + b2), 0, 1), linear ramps lo → fadeLo and fadeHi → hi with ε-inclusive
//                       edges; a1 … b2 runtime params c<chain>_s0 … s3 / v0 … / k0 … the document derives from the zone
//                       (a zone drag is a param write; Degree key zones are in degrees, Hz key zones in ln Hz). A zone
//                       that is off (its whole range) has no node.
// Rack params ("macro1" … "macro16", "select"): a root param d<rack>__<name> (key dev<rack>:<name>) when the rack reads it
// (a mapping of that macro, a select zone) or a wire modulates it; else none. A macro turn is one param write whatever
// it drives. Mapped params (a MacroMapping onto a device param or a nested rack's param): the macro's value m →
// pos = min + (max − min)·shape(m / 127, curve) (pq/core/automation_curve.h) → the param's knob travel map → the pin
// (nodes NodeRole::Mapping, the mapping's id; constants folded at compile time, the math hoisted to block rate while the
// macro is not audio-rate modulated). A mapped param is no longer a param of its own (no dev<id>:<name>). Wires into a
// mapped param add to the mapped position (Q135's law with the mapping as the centre); wires into a macro modulate the
// macro (and so everything it drives).
//
// ---- Meta modules (TMM.3; model.h "Meta modules", pq/meta/type.h, pq/document/meta.h) --------------------------------
// A chain meta device (Device::patch "meta:<type>") calls its own expansion, `meta_<type>__d<id>__<key>`, its form from
// what reaches its chain input (TMM.10-fix1, pq/document/meta.h chainStages; never from its type or kind: ChainNotes at
// the note source — the input reads the note's signal of its meaning, per-voice out, done —, ChainVoice after a voiced
// stage, ChainAudio after audio or unplugged), built from its own settings (Device::settings), its chain input
// (Device::chainInput) and the static-ness of its params (an automation lane, a modulation wire or a macro
// mapping on one makes it modulated); its params are the device's own (d<id>__<name>, dev<id>:<name>), so automation,
// macros and wires work as for any device. A device whose circuit (stock or user text) has NAMED meta calls
// `name = meta_<type>(…)` calls a specialised copy of its root, `<root>__m<id>_<hash>`, in which every such call (and
// every named helper call on the way to one, copied and renamed the same way) calls the instance's expansion
// `meta_<type>__d<id>_<path>_<key>`, built from the instance's settings (MetaInstance; the type's defaults without
// one), the call's wired param pins (not static) and the voice context (the device is an instrument). The instance's
// params stay unconnected runtime params, flattened d<id>__<path>__<name> (path dots as "__"), linked as
// "dev<id>:<path>.<name>" (ParamRef::metaParam): a face drag is one param write and the unit hash does not change.
// Names carry hashes of everything they depend on, so an unchanged state lowers to the same IR (the track patch is
// shared, its unit not recompiled); data never enters an expansion (MetaData edits never lower). An unnamed meta call
// plays the prelude's default state (pq/meta/registry.h). Resource slots (frame tables, voice taps) come from
// LowerOptions::metaSlotOf and are listed with the instance's data in LoweredProject::metaResources.
// TMM.10 / TMM.10-fix1 (docs/plans/MM.md "Voice continuity"; SPEC §15 2026-11-01 "notes are a signal"): a MIDI track
// whose first active device is a chain meta device taking the note source plays a VOICE RUN: it and the active chain
// meta devices right after it that the per-voice signal reaches (Form::ChainVoice: their chain input is an `in` fed by
// the previous stage's per-voice output) stay per voice, so "Phase accumulator → Wavetable" is a polyphonic
// oscillator. At the run's end the voices get a linear release after note-off (kMetaReleaseSeconds, 5 ms: env = gate ?
// 1 : max(z1(env) − 1/(0.005·sr), 0)), the track's `done` is !gate && env ≤ 0, and they are summed; the devices after
// the run receive audio (a meta device there: ChainAudio). TMM.11: when the run's last stage is of a type that decides
// its voices' end (meta::Type::voiceEnd: the Envelope, a VCA on the run's signal; fix1: also as the only stage), its
// `done` output is the track's `done` and there is no declick ramp (the Envelope's release is the end). A stock
// instrument's voices are summed at its output, so a meta device after one receives audio (the pre-fix1 behaviour;
// QUESTIONS Q?-TMM.10-fix1-a item 2). Inside a rack chain only the first stage takes the note source; the later ones
// receive audio (item 5). Every type plays everywhere (no device errors for a type "in the wrong place").
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
//   Audio         (T5.3) x = clipPlayer(clip: <slot>) (LowerOptions::clipSlotOf; global stereo audio: the engine's player of
//                 that slot's audio sequence, audio_sequence.h; a mono sample is duplicated) → its audio effects →
//                 balance pan (centre 0 dB) and the same three stages as a MIDI track (sends, groups, solo, mute and
//                 audibility as for MIDI tracks). Its role is graph::TrackRole::Audio (no voices, no event slot). The
//                 clips themselves are not in the graph: an audio clip edit (gain, fades, transpose, reverse, warp,
//                 crossfade, bounds) changes only the sequence data (ChangeSet Sequence: no compile); creating the track
//                 or editing its chain compiles its unit. T5.4: the engine's clip player renders the slot's clips
//                 (pq/engine/clip_player.h), published by the app from lowerAudioTrack (audio_sequence.h).
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
#include <deque>
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
#include "pq/meta/registry.h"
#include "pq/meta/type.h"

namespace pq::doc {

// The largest polyphony an instrument's `voices` setting may ask for (T4.12-fix1).
inline constexpr uint32_t kMaxPolyphony = 128;

class DeviceLibrary;
struct ResolvedDevice;
// TMM.3: the meta-module expansions (and specialised device roots) a lowering calls ("Meta modules" below), kept alive
// with it (LoweredProject::metaPatches).
struct MetaPatches {
  std::deque<graph::Patch> patches;              // stable addresses (the callee library points into them)
  std::shared_ptr<const meta::Prelude> prelude;  // its `meta_<type>` patches are in the callee library too
};

struct LowerOptions {
  // The device library (devices.h: every patch a device may name, and what those import). Required.
  const graph::PatchLibrary* library = nullptr;
  // T4.4-fix1: resolves user devices (Device::source; devices.h DeviceLibrary::resolve). Null: user devices are
  // bypassed (listed in missingDevices). The app passes the DeviceLibrary whose library() is `library`.
  const DeviceLibrary* devices = nullptr;
  std::span<const graph::SubcircuitRef> externals; // unused since T3.17-fix2 (the IR calls library patches directly)
  // The note-event slot of a MIDI track (Timeline::eventSource(k)); stable across lowerings (the app keeps the map).
  // kNone (graph::kNone): the track has no slot (more MIDI tracks than the engine has slots): it is lowered without
  // its notes.
  std::function<uint32_t(TrackId)> slotOf;
  // T5.3: the clip-player slot of an audio track (the engine's audio sequence its `clipPlayer` plays; stable across
  // lowerings: the app shares the event-slot allocator). Unset or kNone: the track is lowered without its clips (its
  // effects still run) and listed in LoweredProject::withoutSlot.
  std::function<uint32_t(TrackId)> clipSlotOf;
  float smoothingMs = 2.0f;                        // gain smoothing time constant
  // T2.6-fix1: the index into Resources::tunings of the tuning node's table for main tuning `id` with definition
  // `def` (always one with a signal). An index whose entry is null is allowed (the node then passes the static
  // pitches through). Unset: no tuning nodes (every note plays its static Hz, as before T2.6-fix1).
  std::function<uint32_t(TuningId, const TuningDef&)> tuningTableOf;
  // TMM.3: the slot id of resource `index` (meta::ResourceRequest::index) of the meta instance (device, path) — stable
  // while the instance exists (the link owns the slots, TMM.2 / TMM.5). Unset: 0.
  std::function<uint32_t(DeviceId, std::string_view path, uint32_t index)> metaSlotOf;
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
  // T4.8 (racks; lower.h "Racks"): like Wire, a chain id or a mapping id is no other entity's id, so their nodes take
  // the k slots of all three role spaces (k < 48). A rack is a device whose own Device slots stay unused (it has no
  // call): its nodes take the TuningNode and SendInput slots of its id (k < 32: k / 16 picks the space).
  Chain = 4,
  Mapping = 5,
  Rack = 6,
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
// TMM.10-fix2: a wire from a MIDI track's NOTE SOURCE (Wire::noteSource; its port a model.h kNoteOutputs entry) is the
// same math with src = that track's voice terminal (NodeRole::Wire slot kWNote): "hz" the pitch the devices hear (the
// last tuning node's `hz` when the patch has tuning nodes, else voice.hz), "gate" gate_to_real(voice.gate) (kWNoteReal),
// "velocity" voice.velocity. Per voice into a device of its own track that runs per voice (the instrument, a meta voice
// run's stage, a per-voice rack chain's effect); through the wire's reducer into any other device (a global one of the
// track, or across tracks: `export m<wire>` as for an export's wire). Inactive while the track plays no voices (no
// lowered instrument at the start of its chain: voice terminals exist only in a voice context). It never closes a
// cycle (the note source depends on nothing); across tracks the cycle-through-two-instruments rule applies as above.
// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"; model.h "Modulation wires"; QUESTIONS Q?-TX.patching-a). The source is ANY output of its device's callee
// (an `out` — not a bool one such as `done` — or an export; a rack has none), the target a real param or a non-const
// real `in` of the target's callee. Travel wires (migrated, schema ≤ 12) keep T4.7's ends exactly (an export into a
// param; anything else is Invalid), so an old project lowers to the same IR. Add wires (every new wire):
//   param   value = clip(centre + Σ src_i, lo, hi) in the param's normalised unit (Hz, seconds, linear gain for a dB
//           param, cents …; lo / hi its declared range; no clip for a param marked @wrap — a phase its patch wraps;
//           TMM.18: ±max(|lo|, |hi|) for a param marked @signed — the Phase accumulator's freq, a negative
//           frequency running its phase backwards),
//           rounded for @steps; centre = the knob (the root param d<id>__<name>, param rate) or the mapped value, or the
//           Travel wires' result when a param has both kinds. No amount param, no multiply. The tap is then in units:
//           (max(v − centre, 0), max(centre − v, 0)) (ModulationTap::units; the views convert it to knob travel).
//   input   the pin reads what it read before (the chain's signal) + Σ src_i in id order; an input nothing drove takes
//           the wires alone (an unpatched input's default is what it reads only while no wire reaches it). On the
//           source's own track the source keeps every channel (a stereo output into a stereo input); across tracks
//           its first channel (export m<wire> / in m<wire>, as for params). No tap.
//   units   a param whose patch adds its wires after its own law — an `in <param>_add` beside it (the VCA's `value`,
//           meta::ParamSpec::unitsInBody: the knob is a position, the wire adds to the multiplier) — takes them there:
//           the param stays the knob (param rate).
//   meta    a chain meta device's inputs that Add wires reach become `in`s of its expansion (meta::ExpandContext::
//           wired: an unwired chain keeps its expansion and key; under ChainNotes the chain input reads the note's
//           signal plus the wires).
// Audio rate end to end: a wire's signal reaches the pin as it is (the compiler raises the param's path to audio rate
// where the source is; an unwired param stays a param-rate runtime param, its memoised path unchanged).
// FREE devices (model.h Device::free): called in their track patch after the series (TrackDevs::free), never chained:
// a stock device's first audio `in` reads −0 until a wire reaches it; a meta device plays its Nested form (every input
// an `in` with its default, outputs of any poly) in the track's voice context when the track plays voices, so the
// compiler runs it per voice exactly when a per-voice signal reaches it or its output feeds voice processing (pq/graph/
// infer.h free-running sources); wires out of it into global targets go through their reducer (a reducer of a global
// value is the value). A free device nothing reads is dropped by the compiler (its face's voice tap keeps a meta one).
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
  // TX.patching: the tap is in the param's own units (a site with an added wire: how far the value moved), not in knob
  // travel (a site with migrated Travel wires only).
  bool units = false;
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
  std::vector<TrackId> withoutSlot;                           // MIDI / audio tracks lowered without notes / clips (no slot)
  std::vector<DeviceId> missingDevices;                       // bypassed: patch not in the library (or not its kind)
  std::vector<WireState> wires;                               // T4.7: every wire of the project, in id order
  std::vector<ModulationTap> taps;                            // T4.7: one per modulated param (also graph.taps)
  // T4.4-fix1: user devices. With any, graph.library is `userLibrary` (their renamed patches, then the library's and
  // the files their texts import), and the resolutions stay alive with the lowering (the compiled units' instances
  // point into them).
  std::shared_ptr<const graph::PatchLibrary> userLibrary;
  std::vector<std::shared_ptr<const ResolvedDevice>> userDevices;
  // User devices whose current text does not play (ResolvedDevice::error): stale = their last good text plays, else
  // they are bypassed (and in missingDevices).
  struct DeviceError {
    DeviceId device;
    std::string message;
    bool stale = false;
  };
  std::vector<DeviceError> deviceErrors;
  // TMM.3 ("Meta modules" above): every resource the project's meta instances read (per track, device and path: the
  // slots the link must create or keep, and the data to build them from), and the expansions the graph calls.
  struct MetaResource {
    TrackId track;
    DeviceId device;
    std::string path;
    std::string type;
    meta::ResourceRequest request;
    uint32_t slot = 0;
    meta::DataPtr data;  // the instance's (or the type's default)
    std::vector<uint8_t> modulated;  // TMM.8: per param of the type, 1 = modulated (its table must not bake it)
    // (TMM.8-fix: LowerOptions::metaSlotOf is asked for index | variant << 16, meta::Type::tableVariant; `request.index`
    // is the plain index.)
  };
  std::vector<MetaResource> metaResources;
  std::shared_ptr<const MetaPatches> metaPatches;
};

// T4.7: how every wire of `project` is lowered with `library` (the same answer lower() gives, without building the
// graph; the views draw the delayed and inactive ones). In id order. T4.4-fix1: with `devices`, user devices play
// their own patches (a wire to a port an edit removed is Invalid).
std::vector<WireState> wireStates(const Project& project, const graph::PatchLibrary& library);
std::vector<WireState> wireStates(const Project& project, const DeviceLibrary& devices);

// T4.7: the knob travel of a device param — what its panel knob shows (pq/views/devices/device_model.h toPosition /
// fromPosition follow the same rule) and what modulation adds to: Log for @curve(exp) with positive ends and for dB
// gains with a positive floor (linear in dB), else Linear; @steps (and integer ranges with them) round the value.
// TMM.3: `@curve(exp0, k)` (the Phase accumulator's frequency) is exponential from exactly lo: value =
// lo + (hi − lo)·(2^(k·p) − 1)/(2^k − 1) (exp0 = k; 0 for the other laws).
struct ParamTravel {
  bool log = false;
  double lo = 0.0, hi = 1.0;
  bool round = false;
  double exp0 = 0.0;
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
// T4.4-fix1: the same with user devices resolved through `devices` (its library()).
std::vector<LoweredParam> lowerParams(const Project& project, bool tuningNodes, const DeviceLibrary& devices);

// T4.8: a rack's lowered subcircuit on its own — `patch AudioEffectRack<id> { in in; … out out }` (an Instrument Rack:
// no input, voices inside, `done`) built by the same code that lowers it inside its track (chains, zones, chain mixers,
// mappings; its macros and selector as params of the patch). The circuit view of a rack (read-only, follows the
// document while open; Q?-T4.8-a).
// Wires into the rack are not part of it (they belong to the track patch). With `devices`, user devices inside it
// (T4.4-fix1) call their own renamed roots as lower() does (else they are left out, as lower() bypasses them without
// one). Errors: NotFound (no such rack).
Result<graph::Patch> rackSubcircuit(const Project& project, DeviceId rack, const graph::PatchLibrary& library,
                                    const DeviceLibrary* devices = nullptr);

// T4.8: the derived params of one rack chain as lower() names them (its mixer gains "chain<id>:gl" / ":gr"; its zones
// that are on, ":s0" … ":s3", ":v0" …, ":k0" …) with their current values; empty when there is no such chain. The link's
// fast path for a chain mixer or zone drag (`track` is 0: unknown here).
std::vector<LoweredParam> chainParamValues(const Project& project, ChainId chain);

// What, besides structural changes, decides the lowered graph: the sends that are lowered only while stored (return
// → return sends, sends that close a loop) and which of them are delayed. A send level edit that changes this key
// (a return's send to another return turned up from −∞, or back) needs a compile (the link compares it on every
// Param change).
std::vector<uint64_t> conditionalSendsKey(const Project& project);
// The sends lower() gives an automatic 1-sample delay (LoweredProject::delayedSends), without lowering the graph.
std::vector<DelayedSend> delayedSends(const Project& project);

// The key of a document param, or "" for params that are not lowered (solo: derived into the gates; a device param,
// whose key needs its name: the lane form below; a chain's mixer as a whole or a zone, several derived params).
// T4.18: a chain's volume / pan "chain<id>:volume" / ":pan" (lowered while automated), a wire's "wire<id>:amount".
std::string paramKey(const ParamRef& ref);
// T4.18: the key an automation lane drives: paramKey(lane.param), and a device param's "dev<id>:<paramName>" (a key the
// lowered patch declares only while the param is a runtime param of its own: not macro-mapped, a rack param only while
// the rack reads it).
std::string paramKey(const AutomationLane& lane);
// T4.18: the lowest id of the automation lanes of chain `chain`'s volume / pan on `t` (the rack's track), or invalid:
// with one, the chain lowers its volume and pan as params of their own (see "Racks" above), the law's nodes keyed by
// that lane id (nodeIdFor(NodeRole::Chain, lane id, k): a lane id is no other entity's id, like a chain's).
LaneId chainMixerLane(const Track& t, ChainId chain) noexcept;

// The main tuning of a MIDI track's track-level effective tuning (effectiveTuningRef without a clip) when that is a
// main tuning with a linear form (hasTuningSignal); invalid otherwise.
TuningId trackTuningSignal(const Project& project, const TrackPath& path) noexcept;
// T2.6-fix3 (M3 review H4a): the tuning signals a MIDI track's clips follow, one tuning node each, in node order:
// the track-level one (trackTuningSignal) first, then the main tunings with a signal that clips override to, by
// tuning id. A note following signal t is voice.tuning = tuningNodeIndex(signals, t) (1-based; 0: none).
std::vector<TuningId> trackTuningSignals(const Project& project, const TrackPath& path);
uint32_t tuningNodeIndex(std::span<const TuningId> signals, TuningId tuning) noexcept;

}  // namespace pq::doc
