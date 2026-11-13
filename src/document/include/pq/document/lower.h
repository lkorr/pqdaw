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
//
// ---- Racks (GU.9; model.h "Racks") --------------------------------------------------------------------------------
// A rack is a group of parallel chain groups wired by ordinary wires: it lowers as every group does (no path of its own).
//
// ---- Meta modules (TMM.3; model.h "Meta modules", pq/meta/type.h, pq/document/meta.h) --------------------------------
// A chain meta device (Device::patch "meta:<type>") calls its own expansion, `meta_<type>__d<id>__<key>`, its form from
// what reaches its chain input (TMM.10-fix1, pq/document/meta.h chainStages; never from its type or kind: ChainNotes at
// the note source — the input reads the note's signal of its meaning, per-voice out, done —, ChainVoice after a voiced
// stage, ChainAudio after audio or unplugged), built from its own settings (Device::settings), its type's chain input
// and the static-ness of its params (an automation lane, a modulation wire or a macro
// mapping on one makes it modulated); its params are the device's own (d<id>__<name>, dev<id>:<name>), so automation,
// macros and wires work as for any device. Names carry hashes of everything they depend on, so an unchanged state lowers to the same IR (the track patch is
// shared, its unit not recompiled); data never enters an expansion (MetaData edits never lower). An unnamed meta call
// plays the prelude's default state (pq/meta/registry.h). Resource slots (frame tables, voice taps) come from
// LowerOptions::metaSlotOf and are listed with the instance's data in LoweredProject::metaResources.
// VOICE CONTEXTS (IG.1-2; SPEC §15 2026-11-04 "instrument groups", 2026-11-06 "single-signal cables"; docs/plans/IG.md;
// pq/document/instrument_groups.h). What runs per note is decided by structure alone: a block inside an INSTRUMENT
// GROUP (a group whose main input is fed notes: it heads a MIDI track's path, or a notes cable enters it) runs per note
// of that group's notes source (GU.f7: the lone stock instruments that were contexts of their own are gone). Everything
// else is single-signal. (The TMM.10 voice runs — meta modules continuing the notes along a
// path, the ChainNotes / ChainVoice forms on a track, "a stock device ends the voices" — are gone: a meta module on a
// path is ChainAudio, global; heading a MIDI track's path it reads the NEWEST note, voice_last of the signal of its chain
// input's meaning. Rack chains keep their forms.)
//   lowering  a context is lowered in its SOURCE's track patch (its voices are that source's VoiceEngine's). When that
//             is another track than its own, its block and members are HOSTED in the source's patch; on its own track a
//             REMOTE placeholder stands on the path, its output imported (`export v<device>` → `in v<device>`, an Audio
//             wire); every cable between it and its own track crosses as any cross-track wire (export / in m<wire>).
//             One notes source per context: a notes cable from another MIDI track (lowest wire id) wins over the
//             block's own track; any other notes into it are Inactive. (TX.cross-track's guests — g<e>_<o>,
//             gp<X>_<pin>, phantoms, the 8-pin limit, the TwoInstruments refusal — are gone.)
//   group     its call is GroupNotes: In gives the note's Hz / phase by the meaning of the input it reaches, Out is
//             per voice.
//             Members run per note unless every input they have from inside the group is SINGLE-SIGNAL — a cable
//             marked single (Wire::single, reduced by its reducer) or one from a member that runs once — then once
//             (LDev::voice false, a fixpoint over the cables: Callees::perNote). A per-note member's single-signal
//             inputs (and every signal from outside) are BROADCAST: the value marked shared, so a free-running source
//             behind it keeps one instance. A cable from a per-voice source to anything not per note of the same
//             context (outside, or single-signal) reduces by its reducer (sum, max = voice_highest, newest =
//             voice_last). The group's `y` (what its main output gives) = voice_sum of Out's per-note terms (× the
//             release ramp) + Out's single-signal terms added once (LWire::outGlobal).
//   end       a group's voices end when every ender is done: the device whose output alone reaches Out's per-note
//             terms (through pass-throughs: TX.scope-sound) when it is a voiceEnd meta type (the Envelope). Without
//             an ender reaching Out the voices get the 5 ms linear release after note-off (kMetaReleaseSeconds: env =
//             gate ? 1 : max(z1(env) − 1/(0.005·sr), 0), done = !gate && env ≤ 0) and that ends them. The track's
//             `done` is the AND of every context lowered in its patch.
//   voices    a track plays voices when its notes are played or read anywhere (a context's source, a note output wire,
//             a notes cable into a meta module, a meta module heading its path); a unit whose patch then schedules no
//             voice stage runs as a plain unit (project_compile). Its polyphony is the largest of its groups' `voices`
//             settings (mono: 1); its stealing the first group's `steal` setting
//             (ProjectTrack::steal; model.h "Instrument groups").
//   loops     a delay-free loop entirely inside a context (per voice) or entirely outside plays (Delayed, z⁻¹); one
//             that crosses a voice sum — out of a group (or through a single-signal cable) and back into per-note
//             blocks — or that would put two instrument tracks into one unit is Refused (VoiceLoop: IG.3).
//
// ---- Signal flow --------------------------------------------------------------------------------------------------
//   MIDI track    its instrument (the first device, normally BasicSynth, the M3 default instrument,
//                 stdlib/instruments/basic_synth.pqp; QUESTIONS Q15 A) → voice_sum → its audio effects = x (mono, or
//                 stereo after a stereo effect) → (without an active instrument: x = 0, no voices, no event slot)
//                   out  = gain(x, (pl·g, pr·g)),  g  = vg · gate · m         (pl, pr: the track's pan law, below)
//                   post = gain(x, (pl·s, pr·s)),  s  = vg · sendGate · m     (export: what post-fader sends read)
//                   pre  = gain(x, (pl·q, pr·q)),  q  = sendGate · m          (export: what pre-fader sends read)
//                 vg = volume ≤ −70 dB ? 0 : 10^(volume/20); m = mute < 0.5 (the activator, a param: its lane plays).
//                 Pan law (TX.pan-laws, Mixer::panMode, the same on every kind of track, model.h PanMode for the
//                 gains): Balance pl = min(1, 1 − pan), pr = min(1, 1 + pan); Constant Power θ = (pan + 1)·π/4,
//                 pl = sin(θ + π/2), pr = sin θ; Linear pl = (1 − pan)/2, pr = (1 + pan)/2 — all block-rate math on the
//                 pan param, folded into each stage's one smoothed gain. True Stereo: x first goes through
//                 x' = gain(x, (cos a, cos b)) + gain(swap(x), (sin b, sin a)), a = max(pan, 0)·π/2,
//                 b = max(−pan, 0)·π/2 (one smoothed 2×2 matrix per sample), and pl = pr = 1. The law is structural
//                 (SetTrackPanMode recompiles that track; the exports stay, so nothing else does).
//   Audio         (T5.3) x = clipPlayer(clip: <slot>) (LowerOptions::clipSlotOf; global stereo audio: the engine's player of
//                 that slot's audio sequence, audio_sequence.h; a mono sample is duplicated). T6.1: with an input
//                 (Mixer::audioIn) x = gain(clips, 1 − monitor) + gain(track.in, monitor), `monitor` a runtime param
//                 "<id>:monitor" (monitorGate; ParamRef TrackMonitor), both gains smoothed (click-free); the engine binds
//                 the input's device channels to `track.in` (LoweredProject::audioInputs; a mono input is duplicated).
//                 Without an input no `track.in`, no gate: nothing is paid. Then → its audio effects →
//                 its pan law and the same three stages as a MIDI track (sends, groups, solo, mute and
//                 audibility as for MIDI tracks). Its role is graph::TrackRole::Audio (no voices, no event slot). The
//                 clips themselves are not in the graph: an audio clip edit (gain, fades, transpose, reverse, warp,
//                 crossfade, bounds) changes only the sequence data (ChangeSet Sequence: no compile); creating the track
//                 or editing its chain compiles its unit. T5.4: the engine's clip player renders the slot's clips
//                 (pq/engine/clip_player.h), published by the app from lowerAudioTrack (audio_sequence.h).
//   Group         x = `in` (stereo: the sum of what is routed into it) → its audio effects, its pan law, the same
//                 three stages.
//   Return        x = the sum of its sends: for each track k that sends to it, `in s<k>` (that track's post or pre
//                 export) → gain_add(acc, s<k>, sg_k), sg_k = send<k> ≤ −70 ? 0 : 10^(send<k>/20). A send at −∞
//                 costs nothing (gain_add passes the accumulator through, in place). Then its audio effects, its pan
//                 law → out / post / pre as a group.
//   Master        x = `in` → its audio effects → its pan law → out = gain(x, (pl·vg, pr·vg)) (the device output).
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
// ":sendGate", ":mute", T6.1 ":monitor" (an audio track with an input); a send "<sender id>:send:<return id>", a param `send<sender id>` of the RETURN's patch; a
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
#include "pq/dsp/curve.h"
#include "pq/document/model.h"
#include "pq/graph/infer.h"
#include "pq/graph/param_curve.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"
#include "pq/meta/registry.h"
#include "pq/meta/type.h"

namespace pq::doc {

// The largest polyphony an instrument's `voices` setting may ask for (T4.12-fix1).
inline constexpr uint32_t kMaxPolyphony = 128;

class DeviceLibrary;
// TMM.3: the meta-module expansions (and specialised device roots) a lowering calls ("Meta modules" below), kept alive
// with it (LoweredProject::metaPatches).
struct MetaPatches {
  std::deque<graph::Patch> patches;              // stable addresses (the callee library points into them)
  std::shared_ptr<const meta::Prelude> prelude;  // its `meta_<type>` patches are in the callee library too
};

struct LowerOptions {
  // The device library (devices.h: every patch a device may name, and what those import). Required.
  const graph::PatchLibrary* library = nullptr;
  // The block library whose library() is `library` (the native engines' descriptions); may be null.
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
  // GU.1: the CurveSlot (engine Resources::curves index) of driven control (device, param): stable while the control is
  // driven (the link owns the slots and publishes LoweredProject::curves into them); 0: none (the `curve` atom is then
  // the identity on the position). Unset: the lowering numbers its driven controls 1, 2, … in LoweredProject::curves
  // order and owns their slots (graph.curveSlots: a compile given no Resources::curves reads them; tests, tools).
  std::function<uint32_t(DeviceId, std::string_view param)> curveSlotOf;
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
//             sends (a delayed send breaks a loop). TX.groups-fix1: CHAIN LINKS (Wire::chain, a grouped series' links)
//             are taken first (in id order), then every other wire (in id order): a chain link stands for a link of the
//             chain, which is in the graph before any wire, so a loop through a grouped series keeps its delay on the
//             wire that had it in the chain.
//   Inactive  an end's device (or track) is deactivated, or its patch is missing: left out until it plays again.
//             TX.groups-fix1: also a wire from a group's In at the note source into an input that reads no note signal
//             (see GROUPS below), and a wire on a port of a switched-off free device other than its pass-through's two.
//   Invalid   the port or param is not one of the patch's (an edited patch lost it), or the param is a bool.
//   Refused   left out, and the status line and the views say why (WireState::reason):
//             VoiceLoop (TMM.13-fix1 H3; IG.1-2: also a cycle through two instrument tracks, formerly TwoInstruments):
//             it would close a loop through per-voice and global signals (a voice
//             reducer inside the loop, e.g. a later effect's output back into a meta voice run's Phase accumulator
//             `hz`), whose 1-sample delay needs the voices and the global part to alternate per sample: the
//             scheduler refuses that (schedule.h step 3), which silenced the whole track.
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
//   input   the pin reads what it read before (the chain's signal) + Σ src_i in id order (TX.groups-fix1: chain links
//           first, as the chain's signal is first); an input nothing drove takes
//           the wires alone (an unpatched input's default is what it reads only while no wire reaches it). On the
//           source's own track the source keeps every channel (a stereo output into a stereo input); across tracks
//           its first channel (export m<wire> / in m<wire>, as for params). No tap.
//   GU.1 (docs/plans/GU.md "Curves"; model.h ControlCurve, WireLaw): a DRIVEN control — a param with a lowered wire,
//           an automation lane (even an empty one: a lane is structural) or a macro mapping — is lowered as its knob
//           POSITION (the root param d<id>__<name>, now in [0, 1] — the mapped position for a mapped param), then
//           stage 1 in position: + Σ amount·src of the Travel wires + Σ src of the Add wires when the control's curve
//           is not linear (dsp::curveIsLinear); then ONE native `curve` atom (pq/dsp/atoms.h; its CurveSlot from
//           LowerOptions::curveSlotOf, listed in LoweredProject::curves) maps the position through the control's
//           curve (clamp / wrap / mirror for a signed curve, the segments, the step, the output conversion); stage
//           2, on a linear curve: + Σ src of the Add wires in the curve's units, clamped to the curve's range (not
//           for wrap; ±max(|min|, |max|) for signed), stepped, then the output conversion (`curve` with convert 0,
//           then db_to_gain: a dB gain adds in dB); stage 3: the migrated Units wires in the DSP units after it all,
//           clamped to the declared range. The tap is the last stage's (ModulationTap: position, curve units or DSP
//           units). An UNDRIVEN control has no op at all: its value is the curve at its position, evaluated on the UI
//           thread (lowerParams: dsp::evalCurve) and written as the runtime param, as before GU.1 (inside instrument
//           groups too). A rack's params (macros, the selector) are not curves: values 0 … 127, lowered as before.
//   units   a param whose patch adds its wires after its own law — an `in <param>_add` beside it in its text — takes
//           its Add and Units wires there: the param stays the knob (param rate). (GU.4: the VCA, whose `value` was the
//           one meta param of this kind, is a group preset now: in → mul ← a value block with the VCA's curve.)
//   meta    a chain meta device's inputs that Add wires reach become `in`s of its expansion (meta::ExpandContext::
//           wired: an unwired chain keeps its expansion and key; under ChainNotes the chain input reads the note's
//           signal plus the wires).
// Audio rate end to end: a wire's signal reaches the pin as it is (the compiler raises the param's path to audio rate
// where the source is; an unwired param stays a param-rate runtime param, its memoised path unchanged).
// THE MAIN PATH (TX.all-free; SPEC §15 2026-11-03 "everything is free; the chain is just cables"; main_path.h): a
// track's blocks are a set and its series is ordinary cables. The lowering follows the main links from the source
// (doc::mainPath) and compiles the blocks on that path exactly as the chain was compiled (the same IR, the same NodeIds:
// a schema-16 project migrates to the same patch, tests/app/test_all_free.cpp); the path's links are consumed by that
// (they are not lowered as wires: their state is Lowered while the path runs, Inactive past a break). Every other wire
// is lowered as a wire (its "" main ports resolved to the block's main output / input, model.h kMainPort), and every
// block off the path as below. The MIXER takes the path's end when the path reaches it (else nothing: a broken path is
// silent past the break, its mixer input a constant 0) plus Σ the other wires into it (Wire::mixer; its own track's
// blocks; TX.all-free-fix1: another track's too, below; one into it that would close a delay-free cycle gets the
// 1-sample delay; a per-voice source goes through the wire's reducer). A wire from a track's SOURCE with port "" is that
// track's own signal (an audio track's clips, a group's / return's summed input; a MIDI track's notes: a notes cable,
// below).
// NOTES CABLES (TX.all-free-fix1; GU.f7: the stock instruments and racks that played them are gone): a notes cable (a
// wire from a MIDI track's source, port "") into an instrument group's main input plays it (instrument_groups.h). A notes
// cable into an input of a meta module off the path runs it in the track's voice context: a Frequency input reads the
// note's Hz (the tuned pitch, as a note source's "hz" wire), a Phase input its phase (phasor of it, NodeRole::Wire slot
// 15), a Gate or Signal input its own default (the voice's gate, or the default: as a chain input at the note source);
// it holds no voice (no `done`). Into anything else (a param) a notes cable is Inactive. Several voice contexts lowered
// in one patch share the track's voices; its `done` is then the AND of theirs (nodeIdFor(Device, id, 7); none when one
// has none: the engine's own end).
// A cable into ANOTHER TRACK'S MIXER (from a block or a track's own signal) is a send-like route: its every channel (voices
// summed by its reducer) leaves as `export m<wire>`, arrives as `in m<wire>` (width inferred) and is summed into that
// mixer; graph::WireKind::Audio between the tracks. TX.cross-track (SPEC §15 2026-11-03 "tracks are not walls"): one that
// closes a cycle between tracks (the target's output reaches the source's track through routing, sends or other
// cross-track wires) is planned like every wire: Delayed (its z⁻¹, per sample) when the loop is delay-free, Refused only
// through two instrument tracks or across a voice sum (VoiceLoop). See "Loops between tracks" below.
// LOOPS BETWEEN TRACKS (TX.cross-track; SPEC §15 2026-11-03 "tracks are not walls"): the project is one circuit. A wire
// that closes a delay-free loop through other tracks — a cable into another track's mixer, a modulation or input wire,
// a send (Plan's sends: a looping send has its z1, never a block delay) — gets the circuit editor's rule: the wire that
// closes it (taken in id order; sends before wires) has the one-sample z1, deterministic, and the loop runs PER SAMPLE:
// the partition merges the loop's tracks into one unit (graph/project.h: cycles between units merge) and the scheduler
// runs the loop's strongly connected part as one per-sample Loop region (graph/schedule.h step 4); the rest of every
// track keeps block processing, and a project without such loops lowers exactly as before. Refused only where the
// engine cannot run it yet: through two instrument tracks (VoiceLoop: one voice table per unit, Q84; QUESTIONS
// Q?-TX.cross-track-a) or through the voices' sum (VoiceLoop: TX.voice-loop).
// CROSS-TRACK NOTES CABLES: see "Voice contexts" above (IG.1-2: a context is lowered in its notes source's patch).
// FREE devices (blocks off the main path; before TX.all-free model.h Device::free): called in their track patch after
// the series (TrackDevs::offPath), never chained:
// a stock device's first audio `in` reads −0 until a wire reaches it; a meta device plays its Nested form (every input
// an `in` with its default, outputs of any poly) in the track's voice context when the track plays voices, so the
// compiler runs it per voice exactly when a per-voice signal reaches it or its output feeds voice processing (pq/graph/
// infer.h free-running sources); wires out of it into global targets go through their reducer (a reducer of a global
// value is the value). A free device nothing reads is dropped by the compiler (its face's voice tap keeps a meta one).
// TX.groups-fix1: a SWITCHED-OFF free device passes what reaches its chain input (a meta module's chain input, else its
// first non-const `in`) to its main output (its first `out`), as the chain passes a switched-off device by — called as
// `pass__<in>__<out>` (in → out, any poly, up to stereo: no state, nothing computed; LDev::bypass), only while a wire
// reads that output. Its other ports' wires are Inactive (it plays nothing else), and so is it without a chain input
// (unplugged) or an output. Before fix1 a switched-off free device was silent: a project with a wire both into its chain
// input and out of its main output now hears the pass-through (QUESTIONS Q?-TX.groups-fix1-a).
// NESTED GROUPS (GU.2; docs/plans/GU.md "Nesting and instrument groups"): only the OUTERMOST group fed notes is an
// instrument group (instrument_groups.h); every device inside it at any depth is its member, a nested group too. A
// nested group that runs per note (Callees::perNote, the same fixpoint as any member) plays groupVoicePatch() (its
// terminals per voice), else groupPatch(); it never voices again (no GroupNotes, no voice sum, no end of its own: an
// Envelope behind a nested group's Out still ends the voices, followed through it as a pass-through). Cables across a
// nested capsule's edge are ordinary wires (single-signal ones too). A group of blocks is the same graph as the blocks
// ungrouped: the group's own call is an identity the compiler flattens (tests/app/test_nested_groups.cpp).
// ATOM AND VALUE BLOCKS (GU.2; model.h "Atom blocks and value blocks"): an atom block calls atomBlockPatch() (devices.h:
// the atom between its `in`s and `out`s), a value block valuePatch() (`y = value`); both flatten, so a module built of
// atom blocks schedules exactly its atoms. A value block's `value` is a control like any device param: undriven a
// runtime param written in units (no op), driven the GU.1 curve atom after the sum of position and wires.
// GROUPS (TX.groups; model.h "Groups"): nothing of their own. A group device is a call of the fixed patch
// groupPatch() (devices.h; `in x`, `in output`, `out y = output`, `out input = x`; the callee library holds it while a
// group plays) at its place in the chain like any audio effect (or free); its members are free devices of the track,
// called as above. Its In terminal is the source port `input` (what reaches the group: the summed audio a stock effect
// there would receive; nothing for a free group), its Out terminal the input `output` (the Add law: the sum of the
// wires into it); every wire, inside the capsule or across its edge, is an ordinary wire, so a grouped series is the
// same graph as the series with the chain's links as wires: tests/app/test_groups.cpp renders both bit for bit
// (interpreter and JIT). A wide signal (or one whose width only inference knows: the In terminal) into a meta module's
// input arrives as its mid (l + r) / 2, as the chain gives it (effectCall); on a mono signal that is the signal itself.
// NZ.1: an UNFED group — a free one (off its track's path; not an instrument group, which its notes play) with no wire
// into its main input, or only wires from the In of a group that is itself unfed — carries nothing at its In (a group on
// the path is fed by the path, wired or not): a wire from that In into a native engine's input is Inactive (planWires), so the input
// reads its own default as a free engine's inputs do (a free Noise module's clock is unwired; a free Envelope module's
// `in` reads 1, the envelope itself). Into any other block such a wire still delivers the In's zero.
// TX.groups-fix1 (SPEC §15 2026-11-01 "notes are a signal"; QUESTIONS Q?-TX.groups-fix1-a): the In terminal carries what
// reaches the group's place — its chain stage (meta.h chainStages: a group passes Notes / Voices on as Voices) picks
// what it plays (Callees::groupCallee):
//   Audio    groupPatch() (`global` ports: as above, unchanged).
//   Voices   groupVoicePatch() (the same ports without the `global` bound): In is the previous stage's per-voice signal,
//            Out passes the voices on; the group is a stage of the meta voice run (LDev::metaVoice: its first `in` x
//            takes the run's signal, its `y` gives it on), and the run goes on after it.
//   Notes    groupNotesPatch() at a MIDI track's note source: the group is the track's INSTRUMENT (lowerList: it takes
//            the notes as a meta device there does; free members then run in the voice context). A wire from its In
//            gives the input it reaches the note's signal of that input's meaning (meta::Reads, planWires): a
//            Frequency input the note's Hz (`input` = voice.hz inside the group's call: the tuned pitch), a Phase input
//            the note's phase (`phase` = phasor(voice.hz), 0 at note-on), anything else nothing (Inactive: an
//            Envelope's `in`, a param, a stock device's input) — exactly what a chain input at the note source reads.
// A voice run ending in a group ends as the device whose output alone reaches the group's Out (one lowered wire from a
// device of the track, not delayed) would end it: by its `done` when its type decides its voices' end (the Envelope),
// else 5 ms after note-off (the run's release ramp on the group's `y`); its width is that device's. So a grouped stretch
// of a voice run is the run's graph with its links as wires: rendered bit for bit equal, interpreter and JIT
// (tests/app/test_groups.cpp, "groups-fix1"). A hand-built group there follows the same rules whatever is inside it (a
// stock device inside sums what reaches it, as everywhere; ops::groupInCircuit only groups stretches of the run).
// OVERSAMPLED GROUPS (TX.group-oversample; model.h "Oversampled groups"): every device inside a group whose
// `oversample` setting is above 1× (at any depth), and the group's own call, is computed in one oversample region of
// the track patch (graph::OversampleRegion, PATCH_LANGUAGE §2.3) — one region per OUTERMOST oversampled group, its factor
// the largest of the oversampled groups nested in it (regions do not nest). Inside: the devices' own nodes (their call,
// their input adapters), the nodes of a wire between two devices of the region, and the nodes of a modulated param of a
// device inside (the knob, the wires' sum, its curve: at N×, so a loop through a delayed wire between members stays
// inside); a notes cable into a phase input inside computes the note's phase inside. Outside (base rate): the group's
// voice sum, release ramp and ends, panel taps, every wire to or from outside the region (its own nodes), sends,
// mixers. infer() puts the converters on every AUDIO-rate wire across the edge (polyphase IIR half-bands; a bool held /
// any); params and constants — an undriven control, a literal value block — pass as they are, so a knob costs nothing
// more. A device whose callee has a region of its own (the Wavetable's live path or Shape mode) cannot be inside
// another: it stays outside and runs at its own factor (its wires to the members cross down and up). A feedback loop
// through the group's edge (a member's output back into the group from outside) is refused by the compiler (a loop
// must be entirely inside a region or outside it). Instrument groups: the region runs per voice (8 voice lanes per
// converter). Latency: groupOversampleLatency, reported by the converters and compensated by the project's PDC (to the
// nearest sample; pq/graph/latency.h, pq/engine/latency_plan.h).
enum class WireStatus : uint8_t { Lowered, Delayed, Inactive, Invalid, Refused };
// (TX.all-free-fix1's Cycle — a cable into another track's mixer closing a cycle between tracks — is gone: TX.cross-track
// lowers that loop with its z⁻¹.)
// IG.1-2: VoiceLoop is every loop the engine cannot run per sample yet (IG.3): across a voice sum (a single-signal path
// back into per-note blocks, a cable leaving a group back into it) or through two instrument tracks (one voice table
// per unit); TX.cross-track's TwoInstruments is folded into it.
enum class WireRefusal : uint8_t { None, VoiceLoop };  // why a Refused wire is (see above)
struct WireState {
  WireId id;
  WireStatus status = WireStatus::Lowered;
  WireRefusal reason = WireRefusal::None;  // Refused only
  friend bool operator==(const WireState&, const WireState&) = default;
};
// A modulated param's tap: the export `port` of graph track `track`.
struct ModulationTap {
  DeviceId device;
  std::string param;
  uint32_t track = 0;  // index into graph.tracks
  std::string port;    // "tap_d<id>_<param>"
  // GU.1: what the tap measures — how far the wires moved the control up and down — in knob POSITION (stage 1: Add
  // wires on a curved control, Travel wires; not clamped), in the curve's UNITS (stage 2: Add wires on a linear curve;
  // dB for a dB gain), or in the DSP units after the whole curve (stage 3: migrated Units wires).
  enum class Domain : uint8_t { Position, Curve, Dsp };
  Domain domain = Domain::Position;
};

struct LoweredParam {
  std::string key;      // "<track id>:<name>"
  uint32_t track = 0;   // index into graph.tracks
  std::string name;     // the param's name in the track patch
  float value = 0.0f;   // current value in the param's unit
  std::optional<ParamRef> ref;
  // GU.1: a driven control's knob (lower.h "Modulation wires"): `value` is its knob POSITION, the `curve` atom maps it
  // (an undriven device param's value is the curve at its position, in the param's units). Automation writes positions
  // into exactly these.
  bool position = false;
};

// GU.1: a driven control's curve (lower.h "Modulation wires"): the CurveSlot its `curve` atom reads (LowerOptions::
// curveSlotOf; 0: none, the atom is then the identity) and the curve to publish into it.
struct CurveSite {
  TrackId track;
  DeviceId device;
  std::string param;
  uint32_t slot = 0;
  dsp::Curve curve;
  // TX.mod-bands: the node (of the patch the control's nodes are lowered in: its NodeId) and output carrying the
  // control's final value — after every stage, what reaches the device's pin; the response displays hold it at the
  // live modulated value (graph::LinearResponse "SET PORTS").
  graph::NodeId value;
  uint16_t valuePort = 0;
};

// A send lowered with an automatic 1-sample delay because it closes a delay-free loop (see the file comment).
struct DelayedSend {
  TrackId from, to;
};

// T6.1: an audio track whose patch reads `track.in`, and the device input channels the engine binds to it (T6.7).
struct AudioInputBinding {
  TrackId track;
  AudioInput input;
};
// The value of an audio track's monitor gate "<id>:monitor" (lower.h "Signal flow": Audio): In 1, Off 0, Auto the arm.
// T6.7's engine refines Auto with the transport (armed and stopped or recording).
float monitorGate(const Mixer& m) noexcept;

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
  std::vector<CurveSite> curves;                              // GU.1: one per driven control (its `curve` atom's slot)
  std::vector<AudioInputBinding> audioInputs;                 // T6.1: the audio tracks reading `track.in`
  // The combined library graph.library points into when the lowering added patches of its own (the native engines'
  // expansions, block patches LowerOptions::library lacks); null: graph.library is LowerOptions::library.
  std::shared_ptr<const graph::PatchLibrary> userLibrary;
  // Blocks that do not play (a native engine whose expansion failed): `message` says why (`stale` is always false).
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

// ---- Control curves (GU.1; docs/plans/GU.md "Curves"; model.h ControlCurve; pq/dsp/curve.h) ------------------------
// The curve a param's declaration gives: its range ([lo..hi]; none: [min(0, default), max(1, default)]), its @curve
// preset (`linear`; `octave` — or its old name `exp` — with lo > 0; `exponential` / `logarithmic` [, k]; `exp0`, k:
// exponential from exactly lo, the Phase accumulator's frequency), its unit (@unit or the literals': a dB gain with a
// positive floor is a curve in dB with the DbToGain output; Hz, s, %, cents the readout), @steps (step 1), a bool (0 …
// 1, step 1), @wrap (CurveMode::Wrap), @signed (CurveMode::Signed). What every param of the patch language plays unless
// its device stores another (Device::curves).
using graph::declaredCurve;  // dsp::Curve declaredCurve(const graph::ParamDecl&): pq/graph/param_curve.h
// The curve `device` plays for its param `param` declared by `decl`: its own (Device::curves), else the declared one.
dsp::Curve controlCurve(const Device& device, std::string_view param, const graph::ParamDecl& decl) noexcept;
// The knob position of `device`'s param: the stored one, else the declared default's through the DECLARED curve
// (dsp::curvePosition; a curve edit keeps the knob where it is).
double paramPosition(const Device& device, std::string_view param, const graph::ParamDecl& decl) noexcept;
// The param's value in its units: controlCurve at paramPosition (an undriven control's runtime value).
double paramValue(const Device& device, std::string_view param, const graph::ParamDecl& decl) noexcept;
// GU.f4: STRUCTURAL VALUE BLOCKS. A value block whose control is stepped (its curve's step > 0: a menu, a switch) or a
// constant (min == max: a preset's number, such as the 1 in `type − 1` of a menu's mux chain), that
// nothing drives (no wire into it, no automation lane on it) and whose output reaches a choice — a mux / demux `index`
// or a select `cond`, directly or through atom blocks — lowers as the LITERAL of its value instead of a runtime param,
// so the optimiser folds the choice and dead-codes the branches not taken (a Saturator computes its one curve, an LFO
// its one shape). Its value is then structural: SetDeviceParam on it (and a curve edit that makes or unmakes one) is a
// kChangeStructural edit that recompiles, and lowerParams lists no runtime param for it. Same answer for `project` and
// for the project as lowered (main-path links never reach an index input).
// EQ.3: two more structural uses of such blocks —
//   CONST INPUTS  a stepped / constant value block wired straight into an atom block's `real const` input (the svf's
//                 mode and stages from a band's Type and Stages) sets that input to its knob value: a literal at the
//                 call, as a setting (a setting of the same name is then ignored). The block is structural even when
//                 driven: a wire into it or an automation lane on it is IGNORED by the const input (it still drives the
//                 block's output wherever else that goes); lowersAsLiteral says true either way. A wire into a const
//                 input from anything else stays Invalid (left out: the input keeps its setting or default).
//   LITERAL MUX   a mux block whose `index` is one wire from a literal block and whose picked input is one wire lowers
//                 as that wire: the wires out of the mux start at the picked source (the mux block stays, unread). The
//                 choice is made before type inference, so each branch keeps its own width (the EQ's matrix: the
//                 stereo In in Stereo mode, a mono channel in L/R and M/S).
bool lowersAsLiteral(const Project& project, const Device& device);
// The knob curve menus' presets (the "Knob Curve" submenus; the patch language's names): the curves `declared` can
// switch to, in menu order — Octave (min > 0 and not a dB gain, or a declared exp0 curve, which it keeps), Exponential,
// Logarithmic, Linear; none for a stepped or empty range. curveWithPreset builds one over the declared range, unit,
// step, mode and output (a dB gain stays in dB); the declared preset gives the declared curve itself.
std::vector<dsp::CurvePreset> curvePresets(const dsp::Curve& declared);
dsp::Curve curveWithPreset(const dsp::Curve& declared, dsp::CurvePreset preset) noexcept;
// The preset a curve shows in those menus (its own; Custom for a drawn one).
inline dsp::CurvePreset curvePresetOf(const dsp::Curve& c) noexcept { return c.preset; }
// Whether an Add wire into a control on `curve` adds in its units (a linear curve) rather than in position.
inline bool addsInUnits(const dsp::Curve& curve) noexcept { return dsp::curveIsLinear(curve); }
// The declaration of real param `param` of `patch` (null: none, or a bool).
const graph::ParamDecl* findParamDecl(const graph::Patch& patch, std::string_view param) noexcept;
// The declaration of any param `param` of `patch` (bools too; null: none).
const graph::ParamDecl* findAnyParamDecl(const graph::Patch& patch, std::string_view param) noexcept;

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

// What, besides structural changes, decides the lowered graph: the sends that are lowered only while stored (return
// → return sends, sends that close a loop) and which of them are delayed. A send level edit that changes this key
// (a return's send to another return turned up from −∞, or back) needs a compile (the link compares it on every
// Param change).
std::vector<uint64_t> conditionalSendsKey(const Project& project);
// The sends lower() gives an automatic 1-sample delay (LoweredProject::delayedSends), without lowering the graph.
std::vector<DelayedSend> delayedSends(const Project& project);

// The key of a document param, or "" for params that are not lowered (solo: derived into the gates; a device param,
// whose key needs its name: the lane form below). T4.18: a wire's "wire<id>:amount".
std::string paramKey(const ParamRef& ref);
// T4.18: the key an automation lane drives: paramKey(lane.param), and a device param's "dev<id>:<paramName>" (a key the
// lowered patch declares while the param is a runtime param of its own).
std::string paramKey(const AutomationLane& lane);
// GU.1: the key a DRIVEN control's knob position has ("dev<id>:<name>@pos": LoweredParam::position), apart from the
// undriven value's, so an image built before a control became driven (or after it stopped) never reads the other
// meaning; an automation lane on a device param drives that key (paramKey(project, lane)).
inline constexpr std::string_view kPositionKeySuffix = "@pos";
std::string paramKey(const Project& project, const AutomationLane& lane);

// The main tuning of a MIDI track's track-level effective tuning (effectiveTuningRef without a clip) when that is a
// main tuning with a linear form (hasTuningSignal); invalid otherwise.
TuningId trackTuningSignal(const Project& project, const TrackPath& path) noexcept;
// T2.6-fix3 (M3 review H4a): the tuning signals a MIDI track's clips follow, one tuning node each, in node order:
// the track-level one (trackTuningSignal) first, then the main tunings with a signal that clips override to, by
// tuning id. A note following signal t is voice.tuning = tuningNodeIndex(signals, t) (1-based; 0: none).
std::vector<TuningId> trackTuningSignals(const Project& project, const TrackPath& path);
uint32_t tuningNodeIndex(std::span<const TuningId> signals, TuningId tuning) noexcept;

}  // namespace pq::doc
