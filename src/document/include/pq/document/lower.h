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
// ANALYSIS TAPS (TX.spectrum): an active analysed stock device in a chain (doc::analysedDevice: EQ Eight, Auto Filter;
// an edited fork too) gets a sink signal_tap(in[0], in[1], out[0], out[1], slot) next to its call (node ids
// nodeIdFor(Device, id, 8 … 13)), its slot from LowerOptions::metaSlotOf(device, kAnalysisTapPath, 0), listed in
// LoweredProject::metaResources (type "", kind SignalTap): the panel's spectrum and measured response read it. Without
// metaSlotOf (tools, tests), or with slot 0, no tap. Unwatched it costs one relaxed load per block (pq/dsp/atoms.h).
// VOICE CONTEXTS (IG.1-2; SPEC §15 2026-11-04 "instrument groups", 2026-11-06 "single-signal cables"; docs/plans/IG.md;
// pq/document/instrument_groups.h). What runs per note is decided by structure alone: a block inside an INSTRUMENT
// GROUP (a group whose main input is fed notes: it heads a MIDI track's path, or a notes cable enters it) runs per note
// of that group's notes source; a LONE stock instrument or Instrument Rack fed notes is a context of its own (its code is
// per voice). Everything else is single-signal. (The TMM.10 voice runs — meta modules continuing the notes along a
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
//   group     its call is GroupNotes: In gives the note's Hz / phase by the meaning of the input it reaches (a stock
//             instrument member plays the notes through In or a notes cable of the same source), Out is per voice.
//             Members run per note unless every input they have from inside the group is SINGLE-SIGNAL — a cable
//             marked single (Wire::single, reduced by its reducer) or one from a member that runs once — then once
//             (LDev::voice false, a fixpoint over the cables: Callees::perNote). A per-note member's single-signal
//             inputs (and every signal from outside) are BROADCAST: the value marked shared, so a free-running source
//             behind it keeps one instance. A cable from a per-voice source to anything not per note of the same
//             context (outside, or single-signal) reduces by its reducer (sum, max = voice_highest, newest =
//             voice_last). The group's `y` (what its main output gives) = voice_sum of Out's per-note terms (× the
//             release ramp) + Out's single-signal terms added once (LWire::outGlobal).
//   end       a group's voices end when every ender is done: the device whose output alone reaches Out's per-note
//             terms (through pass-throughs: TX.scope-sound) when it is a voiceEnd meta type (the Envelope) or a
//             stock instrument, and every stock instrument / Instrument Rack inside. Without an ender reaching Out the
//             voices get the 5 ms linear release after note-off (kMetaReleaseSeconds: env = gate ? 1 : max(z1(env) −
//             1/(0.005·sr), 0), done = !gate && env ≤ 0) and that ends them (with the instruments'). The track's
//             `done` is the AND of every context lowered in its patch.
//   voices    a track plays voices when its notes are played or read anywhere (a context's source, a note output wire,
//             a notes cable into a meta module, a meta module heading its path); a unit whose patch then schedules no
//             voice stage runs as a plain unit (project_compile). Its polyphony is the largest of its groups' `voices`
//             settings (mono: 1) and its stock instruments' `voices`; its stealing the first group's `steal` setting
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
//                 that slot's audio sequence, audio_sequence.h; a mono sample is duplicated) → its audio effects →
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
//   TX.knob-curves (SPEC §15 2026-11-01 "answers to Q186" + clarification; model.h KnobCurve, WireLaw): the Add law
//           above is now the UNITS law (WireLaw::Units: every TX.patching wire of a schema-13 … 15 file migrates to
//           it, so an old project lowers to the same graph). An Add wire adds BEFORE the knob's curve
//           (paramTravel(decl, Device::curve)): on a CURVED knob (Octave / Exponential / Logarithmic, travelsInUnits
//           false: a cutoff, the Phase accumulator's frequency bar, an envelope time) it joins the Travel wires' sum
//           without an amount — p = clip(pk + Σ amount·travel_src + Σ add_src, 0, 1), value = the curve at p (+1 =
//           the whole range; an @wrap param wraps p with frac, an @signed one reaches −1 … 1 with the curve mirrored:
//           value = sign(p)·curve(|p|)); on a LINEAR knob it adds in its units as above, and on a dB gain in dB:
//           value = e^(clip(ln centre + Σ src · ln 10 / 20, ln lo, ln hi)) (its tap in nepers: ModulationTap::db).
//           The stages: travel (Travel + curved Add), then linear Add, then Units, each centred on the one before;
//           the tap is the last stage's. Travel wires and macro mappings move along the knob's curve too (the
//           declared one unless switched: unchanged for every old project).
//   units   a param whose patch adds its wires after its own law — an `in <param>_add` beside it (the VCA's `value`,
//           meta::ParamSpec::unitsInBody: the knob is a position, the wire adds to the multiplier) — takes them there:
//           the param stays the knob (param rate). TX.knob-curves: a Units wire always; an Add wire only when the
//           knob is linear in that law (meta::Type::unitsLaw: the VCA on its Bend curve), else it adds to the knob
//           position (the VCA on its Log curve: in travel, through the curve).
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
// TX.all-free-fix1 (QUESTIONS Q208, Q?-TX.all-free-fix1-a): STOCK INSTRUMENTS AND RACKS ARE ORDINARY BLOCKS. A stock
// instrument (or an Instrument Rack) is a block whose main input is notes: it plays the notes that reach it by a cable —
// the path's first link on a MIDI track (it heads the path: lowered exactly as before), or a NOTES CABLE (a wire from its
// own MIDI track's source, port "", into its main input; Cables::notesTo) wherever it is: after the head on the path (its
// voices summed, voice_sum NodeId nodeIdFor(Device, id, 6); what reached it in the series is dropped — it has no audio
// input), or off the path (called after the path, per voice; wires from it reduce as from any voice device). One no notes
// reach is not called and is not missing; in a series (the path, a rack chain) what follows it hears silence (kSilent).
// Several such voice consumers share the track's voices; its `done` is then the AND of theirs (nodeIdFor(Device, id, 7);
// none when one has none: the engine's own end), its polyphony the largest `voices` setting. A notes cable into an input
// of a meta module off the path runs it in the track's voice context: a Frequency input reads the note's Hz (the tuned
// pitch, as a note source's "hz" wire), a Phase input its phase (phasor of it, NodeRole::Wire slot 15), a Gate or Signal
// input its own default (the voice's gate, or the default: as a chain input at the note source); it holds no voice
// (no `done`). Into anything else (a stock effect, a param) a notes cable is Inactive. RACKS off the path play by their
// wires: an Instrument Rack by its notes cable, an Audio Effect Rack while a wire reads its output (`out`); a wire into an
// Audio Effect Rack's input (`in`, on the path too) adds to what reaches it (an `add` with −0 at nodeIdFor(Device, rack,
// 0), whose first term the wires replace). Inside a rack each chain is a series with the same rule: an instrument plays
// first in an Instrument Rack's chain (the notes), else nothing (the document no longer refuses one elsewhere in a chain).
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
// GROUPS (TX.groups; model.h "Groups"): nothing of their own. A group device is a call of the fixed patch
// groupPatch() (devices.h; `in x`, `in output`, `out y = output`, `out input = x`; the callee library holds it while a
// group plays) at its place in the chain like any audio effect (or free); its members are free devices of the track,
// called as above. Its In terminal is the source port `input` (what reaches the group: the summed audio a stock effect
// there would receive; nothing for a free group), its Out terminal the input `output` (the Add law: the sum of the
// wires into it); every wire, inside the capsule or across its edge, is an ordinary wire, so a grouped series is the
// same graph as the series with the chain's links as wires: tests/app/test_groups.cpp renders both bit for bit
// (interpreter and JIT). A wide signal (or one whose width only inference knows: the In terminal) into a meta module's
// input arrives as its mid (l + r) / 2, as the chain gives it (effectCall); on a mono signal that is the signal itself.
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
  // TX.patching: the tap is in the param's own units (a site with an added wire: how far the value moved), not in knob
  // travel (a site with migrated Travel wires only).
  bool units = false;
  // TX.knob-curves: units in NEPERS (ln gain: dB × ln 10 / 20) — an Add wire into a dB gain on its linear (dB) curve.
  bool db = false;
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
// TX.knob-curves (model.h KnobCurve): the travel of a knob on curve `curve` — Default the declared law above; Linear:
// linear in the units (a dB gain: Log, linear in dB); Octave: Log (lo > 0), a declared exp0 law kept; Exponential /
// Logarithmic: exp0 = ±kKnobBendK / ln 2 — with `log` (a dB gain) the bend is in dB: value = lo·(hi/lo)^g(p), g the
// bend. A curve the param cannot take (curveAvailable false) resolves to the declared law.
struct ParamTravel {
  bool log = false;
  double lo = 0.0, hi = 1.0;
  bool round = false;
  double exp0 = 0.0;  // ≠ 0: value = lo + (hi − lo)·g(p) (log: lo·(hi/lo)^g(p)), g = (2^(exp0·p) − 1)/(2^exp0 − 1)
  bool db = false;    // TX.knob-curves: a dB gain with a positive floor (log is its declared law: linear in dB)
};
ParamTravel paramTravel(const graph::ParamDecl& decl, KnobCurve curve = KnobCurve::Default) noexcept;
// TX.knob-curves: `declared` (paramTravel(decl)) on curve `curve` (paramTravel(decl, curve) is this).
ParamTravel withCurve(const ParamTravel& declared, KnobCurve curve) noexcept;
// TX.knob-curves: whether the param can take the curve (a continuous range: not @steps, hi > lo; Octave needs lo > 0
// or a declared exp0 law, and is not offered for a dB gain). Default always.
bool curveAvailable(const ParamTravel& declared, KnobCurve curve) noexcept;
// TX.knob-curves: the curve Default means for this param (Octave for @curve(exp) / exp0, else Linear: a dB gain's
// declared law is linear in dB).
KnobCurve declaredCurve(const ParamTravel& declared) noexcept;
// TX.knob-curves: whether a knob on travel `t` is LINEAR in its units (an Add wire adds in units: dB for a dB gain);
// false: curved (an Add wire adds in travel).
bool travelsInUnits(const ParamTravel& t) noexcept;
// TX.knob-curves: the declaration of real param `param` of `patch` (null: none, or a bool).
const graph::ParamDecl* findParamDecl(const graph::Patch& patch, std::string_view param) noexcept;
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
