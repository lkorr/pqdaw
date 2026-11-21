#pragma once

// The arpeggiator's core (AP.1, docs/plans/AP.md "Semantics → Ableton parity", "Pitch units", "Realtime and CPU"):
// a pure, deterministic object — held-note set + params + clock → note events. ArpStage runs it as a note stage
// (note_stage.h) on the audio thread; the same ArpCore runs offline over a clip's notes (AP.7) with segments the
// caller makes up: it reads no engine globals, only its NoteStageBlock (time, frames, clock segments, tempo map).
//
// ---- Held set ------------------------------------------------------------------------------------------------------
// Up to kArpMaxHeld (128) source notes, consumed (they do not sound themselves). Note-ons are held; a note-off
// releases (Hold off) or only lifts the key (Hold on: latched). Hold: a note-on while no key is down replaces the
// latched set; one while a key is down joins it; Hold switched off drops the latched notes no key holds. Events of
// an id the stage does not hold (notes that started before the stage was installed, a 129th note's off) pass through
// unchanged; AllNotesOff / AllSoundOff pass through and clear the held set, the generated notes and the clock. A
// NoteOff flagged kNoteEventGone (a handover: its producer stage was removed) drops the note even with Hold on.
// "Up" orders the held notes by Hz (ties: arrival), so it works in any tuning; Play Order by arrival.
//
// ---- Clock (at sample k of a block: frame order; at one frame: gate ends, then input events, then the step) -----------
// Synced rate (Rate num/den of a whole note × tuplet m/n: straight 1:1, triplet 3:2, dotted 2:3, any n:m) while the
// transport plays: step k starts at song tick k · L (+ Groove on odd k), L exact as a rational of ticks (pq::kPpq),
// converted through the tempo map (ticksToSamplesF, linear inside one tick): playback, render and an offline run
// agree to the sample at any block size. The first step after the held set fills is the first grid step at or after
// the note-on — except when the arp is idle (nothing held, no generated note sounding): then the first step sounds AT
// the note-on (live feel; AP.3, Q?-AP.1-a (3)) with grid index k − 1 of the next grid step k, its gate · L long (a
// gate ≤ 100 % ends it at the next grid step at the latest), and the later steps stay on the grid. A note-on exactly
// on a grid step is that step. Free rate (ms), or a stopped transport: a free clock (Synced: L at the song tempo) whose step 0 is the
// note-on that filled the held set (or the frame the transport stopped at, keys held). Groove g: odd steps start
// g · L / 2 late. Gate 1 … 200 % of L (> 100 %: overlapping notes). Steps that start inside a block use the params
// of that block (param rate).
// Transport: a loop wrap / locate (a Song segment that does not continue the last one) or a stop (Song → Free) ends
// every generated note at that frame and re-seeks the clock; Hold keeps the latched set across a wrap / locate, a
// stop drops the latched notes no key holds. AP.8a (review M3): a tempo edit is not a locate — a block whose clock says
// `remapped` (the timeline swapped the tempo map at frame 0, keeping the tick) ends nothing: the next step re-seeks on
// the same grid index under the new map, each pulse clock resumes at its first unplayed onset, and the sounding
// notes' remaining gate / glide times scale by the tempo ratio (the last block's bpm over this one's).
//
// ---- Pattern --------------------------------------------------------------------------------------------------------
// The cycle is the style's base pattern over the n held notes, played Steps times (copy j = 0 … Steps − 1 shifted by
// j · Distance in the pitch unit: "Shift"; or "Rotate": pattern element i of copy j plays held note (i + j · Distance)
// mod n of the ordered set, up one equave — Degrees: one equave of the table; Ratio: × p/q — per wrap). Offset
// rotates where the cycle starts; Repeats (0 = ∞) ends the arp after that many cycles until a retrigger. The pattern
// restarts (step 0) when the held set fills, on every note-on (Retrigger Note) or at the first step at or after each
// retrigger-rate boundary (Retrigger Beat: song ticks while synced and playing, else the free clock). Random /
// Random Other (a shuffle per n steps) / Random Once (one shuffle per held-set change) draw from a counter RNG keyed
// by (seed, grid step index): the same steps on every run.
// Base patterns for n held notes, indices into the ordered set (n = 4 in brackets, see tests):
//   Up 0…n−1 [0123]  Down [3210]  UpDown [012321]  DownUp [321012]  Up & Down [01233210]  Down & Up [32100123]
//   Converge (outer → inner) [0312]  Diverge (reverse) [2130]  Con & Diverge [031213]  Pinky Up [030313]
//   Pinky UpDown [03132313]  Thumb Up [010203]  Thumb UpDown [01020302]  Play Order (arrival)  Chord Trigger (all
//   notes every step)  Random / Random Other / Random Once (P = n).
// Harmonic Walk (AP.3): the elements are not the held notes but, for each held note taken as harmonic h of its
// anchor (Harmonics unit: its voice.harmonic, else Base), the harmonics h … h + Range over its anchor (Hz · (h + m)
// / h), or with Under the undertones 1/h … 1/(h + Range) (Hz · h / (h + m)). The walks of all held notes are merged
// by Hz (an element within 0.01 ¢ of a lower one is a duplicate: once), at most kArpMaxWalk elements, and played in
// Walk order Up / Down / UpDown / Random (P = W elements; Random draws as Random does). Distance / Steps / Rotate
// work over the W elements as over held notes.
//
// ---- Pitch units (exact identities) ------------------------------------------------------------------------------
// Every generated note is a PITCH IDENTITY = the source's degree identity (degree / equave / tuning, or none) × an
// exact ratio (note_events.h "Pitch identity"): it keeps the source's `tuning` and Degree identity (Degrees: the
// shifted degree) and its Hz is that identity's static Hz times the ratio, so a patch's tuning node (tuning_hz,
// T2.6: E = hz / hz₀) moves EVERY step — copies, Ratio / Harmonics steps, Harmonic Walk elements, Layers, latched
// notes — with a live tuning modulation per sample, at its exact ratio over the moved degree (AP.8a, review H1).
// Degrees: degree D + k of the note's tuning (D = equave · N + degree; the new degree / equave normalized). The Hz
// moves by cents(D + k) − cents(D) of the note's table (ArpTables::of(its tuning): its clip's tuning, AP.8a H4; set
// off-thread with the stage; default 12-EDO). A note without a Degree identity is shifted by the table's interval
// from degree round(noteDegree) (0 when unknown); it keeps its `tuning` (a reference-relative note follows the
// reference). Under a modulated tuning the Hz the table gives is that degree's static pitch (the node's E = 1), so
// the step follows the modulation and an arp glide composes on top.
// Ratio: × (p/q)^e (e = j · Distance, or the Rotate wraps), an exact integer fraction (Hz = hz · num / den in double,
// float32 at the end): ratio = source ratio · num/den, harmonic = h · num/den when that is an integer ≤ 2^24 (else
// 0), the source's degree / equave / tuning (the node: E = the source's E · num/den), noteDegree moved by the table's
// steps (fractional).
// Harmonics (AP.3): a note with voice.harmonic h > 0 (an integer: its number in its JI group, anchor = Hz · h_a / h)
// steps to h + k over the same anchor; a note without one counts as harmonic Base of itself. Hz = hz · (h + k) / h
// (double, float32 at the end), ratio = source ratio · (h + k) / h, harmonic = h + k exactly (≤ 2^24, else 0); the
// source's degree / equave / tuning, noteDegree moved fractionally. h + k < 1 is a rest (no note). Rotate's wrap is
// an octave of the series (h → 2h).
// A copy with no shift carries the source's identity unchanged.
// voice.key (SM.1): a copy keeps the source's key; a step that keeps a Degree identity moves the key by the degrees moved
// (from the source's key; unknown: 60 + noteDegree); any other step takes the nearest 12-TET key of its Hz.
// Tuning edits (AP.2b, AP.8a H2): a new table set (ArpTableMailbox: a definition change, a clip / track tuning
// switch; never a continuous drag, which only moves the node's live inputs) re-resolves every held note — latched
// ones too, whose producer no longer sends retunes — from the old table of its tuning to the new one: a Degree note
// keeps its degree and its ratio to it (Hz × new Hz(D) / old Hz(D)), a reference-relative one moves with the
// reference, a static one stays; then every sounding step re-derives its pitch at frame 0 with a 5 ms retune (the
// running Into / Out glides re-aimed, their glide state moved: review L1). The link posts a table before it publishes
// the retuned sequences / live routes, so a producer's retune (absolute) arrives with or after it.
//
// ---- Glides (AP.3, "Pitch into / out of curves") -------------------------------------------------------------------
// Into: a step's note starts at a source pitch and glides to its own over Time: Step = the previous step's note
// (Chord Trigger: the previous chord's note of the same rank, else its top note; none before the first step of a
// run), Offset = its own pitch moved by Into offset (in the pitch unit, rounded, or in cents with Glide cents).
// Out: over the last Time of the step's gate the note glides to a target: Step = the next step's note (already
// sounding with a gate > 100 %, else the step the pattern plays next, as known at that frame), Offset = its own
// pitch moved by Out offset. Time = Glide ms, or (Glide synced) Glide fraction of the note's length; ≥ 1 frame, and
// when Into + Out exceed the note each gets half. Shape = Glide shape (RampShape) with Glide bend (−1 … 1 → the
// byte −127 … 127): the NoteEvent ramp shape (note_events.h), evaluated by the VoiceEngine. The source note's own
// bend carries over on top: the Into glide aims at the source's bend at its end (a running bend continues from
// there); a source Pitch event during the Into glide re-aims it; during the Out glide the note goes to its target
// (later source bends do not move it).
//
// ---- Pulse mode (AP.4, "Pulse mode (polyrhythm)") -----------------------------------------------------------------
// Pulse on: no pattern order (Style, Offset, Repeats, Groove, Retrigger Beat do not apply). Every held note repeats
// on its own clock with period Base · h_ref / h. Base = the Rate (synced or ms). h: Pulse source Harmonic = the
// note's JI harmonic (an integer ≥ 1), else Proportion = entry r of the Proportion (r = the note's rank by Hz among
// the held notes; beyond its length the list repeats doubled: prop[r mod len] · 2^(r div len)), else Order = r + 1;
// h is clamped to 1 … kArpMaxPulseH. h_ref = Pulse ref (fixed), or 0 = the lowest h held. Phase lock: all clocks
// restart together every Cycle = Base · h_ref (Pulse cycle Base: a note's h pulses fill it exactly) or Pulse bars
// bars of the song's time signature (Bars, AP.2b: cycle c spans bars c · n … (c + 1) · n − 1 of the signature map, so
// a 3/4 or 7/8 bar and a signature change are followed; the free clock uses the bar at the playhead
// (StageClock::beatsPerBar); a clock's last pulse before the restart may be short of a period). Cycles sit on the
// song grid from tick 0 (synced, playing) or start at the note-on that filled the held set (free clock), so pulse j
// of cycle c starts at start(c) + j · Base · h_ref / h, computed from (c, j) as an exact rational of ticks (1/(D · h)) and
// converted once through the tempo map: no accumulated drift, the same samples at any block size, across tempo
// changes and loop wraps. A clock plays at most one pulse per sample (pulses closer than that merge).
// A note-on starts its clock: its first pulse sounds AT the note-on when that is between its grid onsets (the AP.3
// idle rule, per clock: each new note's clock is idle), gate · its period long (gate ≤ 100 %: ended at its next
// onset at the latest), the later pulses on its grid. Any held-set change (a note added, released, retuned) or
// param change re-derives every h, h_ref and Cycle at that frame; each clock continues at its first onset at or
// after that frame not yet played. Transport resets (wrap / locate / stop) re-seek every clock to the grid.
// Per pulse: Gate (of its own period), velocity (source / Fixed, Decay from the clock start), Distance / Steps
// (pulse j of a cycle plays copy j mod Steps, shifted (j mod Steps) · Distance in the unit; Rotate reads as Shift:
// there is no pattern to rotate), the Into / Out glides (Step = the same clock's previous / next pulse) and the
// source's expression.
// Layers (both modes): 0 … kArpMaxLayers extra copies of every generated note at (p/q)^L of it (the Ratio unit's
// p/q: exact ratio / harmonic as the Ratio unit gives them), each at gain Layer level L times the note's gain.
//
// ---- Step lanes (AP.5, "Step lanes") ---------------------------------------------------------------------------------
// Six lanes, each switched on / off with its own length 1 … kArpMaxLaneSteps (structural: a change republishes the
// chain); their step values are runtime params (ArpLaneSteps, or a project param slot per step read only when that
// step plays: a drag or automation never rebuilds anything). Lanes are polymetric: lane position = the grid step
// index kk mod its length (pattern mode: the song grid while synced and playing, else the free clock's step count),
// independent of the pattern's length, the pattern restarts and of the block size; Pulse mode: per note, its pulse
// count c · J + j (cycle c of J pulses, pulse j) mod the length. A lane switched off reads its default.
//   Velocity   × the step's velocity (0 … 1), or the velocity itself with Velocity source Step Lane (lane off: as
//              played); Decay applies after it.
//   Gate       × Gate (0 … 2; the step's gate clamped to 0.01 … 2 of a step / pulse period).
//   Glide      × the Into / Out glides' pitch distance (0 … 1, in log Hz; 0: no glide); a tie: × its glide time
//              (0: a legato jump).
//   Transpose  + steps in the pitch unit (−48 … 48, rounded; Degrees: degrees, Ratio: powers of p/q, Harmonics:
//              harmonics), part of what the step's notes play (a retune and Out → Step follow it).
//   Chance     the probability the step plays (0 … 1): a counter-RNG draw keyed by (seed, kk) (pulse: (seed ^ h,
//              the note's pulse count)), so playback, render and preview agree.
//   Skip / Tie Play | Skip (a rest: the pattern still advances) | Tie: the step's notes continue the previous step's
//              (same rank and layer; pulse: the same clock's) instead of new note-ons: the note's end moves to the tie
//              step's gate end, and with Into on it glides (the glide time × the Glide lane) to the tie step's
//              pitch (else keeps its own). A step followed by a tie holds its notes past the next onset (their Out
//              glide waits for the tie); when the tie does not happen (its chance fails, nothing is held, Repeats
//              ended) they end at that onset. A tie with nothing to continue plays as a normal step.
// Per step the cost is one read per lane that is on (its step's slot) and one RNG draw with Chance on.
//
// ---- Velocity, expression -----------------------------------------------------------------------------------------
// Velocity: as played (the source note's) or Fixed; Decay (ms, 0 = off) moves it linearly to Target from the pattern
// start (and from every retrigger when Velocity Retrigger is on). Expression carry-over: a Pitch / Gain event of a
// held source note applies to every step that note is producing — sounding ones get the event (Pitch: their Hz
// times the source's new bend, same ramp; Gain: the same gain and ramp) and later steps start at the source's
// current bend / gain (inside a ramp: its value at the step, then the rest of the ramp). A retune (kNoteEventRetune)
// re-derives the held note and its sounding steps' identities. Sync / conditional gains of a held note are dropped.
//
// ---- Panel status (AP.6) ------------------------------------------------------------------------------------------
// With a NoteStageStatus set (setStatus; the playback link gives each arpeggiator device one), every step / pulse that
// is due writes each lane's position (lane position mod its length) and the pattern position, and the pattern list is
// written after a block in which the held set or what orders it changed: the entries in play order — the pattern's
// positions (Up, UpDown …: one per position of a period, Harmonic Walk: the walk's), the elements by Hz for Random /
// Random Other / Random Once / Walk Random (the position: the element drawn), the held notes by Hz for Chord Trigger
// (all lit) and Pulse (the position: the pulsing note's rank) — the source notes' Hz, before Distance / Transpose /
// Layers. Nothing held: idle. Cost: a few relaxed stores per step, one O(entries) write per held-set change.
//
// ---- Capacities -----------------------------------------------------------------------------------------------------
// kArpMaxHeld source notes (a note-on beyond: dropped, counted); kArpMaxSounding generated notes (beyond: the
// earliest-ending ones end at the new step's frame, counted). Per block O(events + steps + gate ends in the block);
// AP.8a (review M4): a step (or a frame of pulses) that starts many notes into a full table makes its room in ONE
// partial selection (O(sounding)), and a tie step finds its waiting notes through a (rank | clock, layer) table, so a
// 128-note Chord Trigger with 3 layers and ties costs O(sounding) per step, not O(sounding²). A block with nothing
// held, nothing sounding and no event returns after one compare (ArpStage maps no param for it: review L5).
// Generated ids skip counters still sounding after a 2^24 wrap (review L4).

#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "pq/core/note_stage_status.h"
#include "pq/engine/note_events.h"
#include "pq/engine/note_stage.h"

namespace pq::engine {

inline constexpr uint32_t kArpMaxHeld = 128;
inline constexpr uint32_t kArpMaxSounding = 512;
inline constexpr uint32_t kArpMaxPattern = 4 * kArpMaxHeld;  // Pinky / Thumb UpDown: < 4n
inline constexpr int32_t kArpMaxSteps = 8;
inline constexpr int32_t kArpMaxTuplet = 32;
inline constexpr int32_t kArpMaxRateDen = 128;
inline constexpr uint32_t kArpMaxWalk = 1024;  // Harmonic Walk elements (merged over the held notes)
inline constexpr int32_t kArpMaxWalkRange = 32;
inline constexpr int32_t kArpMaxProportion = 8;   // AP.4: Pulse proportion entries
inline constexpr int32_t kArpMaxPulseH = 1024;    // AP.4: h and a fixed h_ref
inline constexpr int32_t kArpMaxPulseBars = 16;   // AP.4: a Bars cycle
inline constexpr int32_t kArpMaxLayers = 3;       // AP.4: extra copies per note
inline constexpr int32_t kArpMaxLaneSteps = 32;   // AP.5: steps per lane

enum class ArpStyle : uint8_t {
  Up, Down, UpDown, DownUp, UpAndDown, DownAndUp, Converge, Diverge, ConAndDiverge, PinkyUp, PinkyUpDown, ThumbUp,
  ThumbUpDown, PlayOrder, ChordTrigger, Random, RandomOther, RandomOnce, HarmonicWalk,
  Count
};
enum class ArpRetrigger : uint8_t { Off, Note, Beat };
enum class ArpUnit : uint8_t { Degrees, Ratio, Harmonics };
enum class ArpWalkOrder : uint8_t { Up, Down, UpDown, Random };
enum class ArpGlide : uint8_t { Off, Step, Offset };  // Into: Step = the previous step; Out: Step = the next step
enum class ArpTranspose : uint8_t { Shift, Rotate };
enum class ArpVelocitySource : uint8_t { AsPlayed, Fixed, Lane };  // Lane (AP.5): the Velocity lane's step
enum class ArpPulseSource : uint8_t { Harmonic, Proportion, Order };  // AP.4: where a pulse clock's h comes from
enum class ArpPulseCycle : uint8_t { Base, Bars };                    // AP.4: the phase-lock cycle
// AP.5: the step lanes (arp.h "Step lanes"), and what a Skip / Tie step does.
enum class ArpLane : uint8_t { Velocity, Gate, Glide, Transpose, Chance, SkipTie, Count };
inline constexpr uint32_t kArpLaneCount = static_cast<uint32_t>(ArpLane::Count);
enum class ArpStepKind : uint8_t { Play, Skip, Tie };

// A step length: Synced num/den of a whole note × m/n (n notes in the time of m), or Free `ms`.
struct ArpRate {
  bool synced = true;
  int32_t num = 1, den = 16;          // 1 … 16 / 1 … kArpMaxRateDen
  int32_t tupletN = 1, tupletM = 1;   // 1 … kArpMaxTuplet: straight 1:1, triplet 3:2, dotted 2:3, quintuplet 5:4
  float ms = 125.0f;                  // Free: 1 … 10000 ms
  friend bool operator==(const ArpRate&, const ArpRate&) = default;
};

struct ArpParams {
  ArpStyle style = ArpStyle::Up;
  ArpRate rate;
  float gate = 0.5f;                  // 0.01 … 2 of a step
  float groove = 0.0f;                // 0 … 1: odd steps groove · L / 2 late
  int32_t offset = 0;                 // cycle rotation (steps)
  int32_t repeats = 0;                // cycles, 0 = ∞
  ArpRetrigger retrigger = ArpRetrigger::Off;
  ArpRate retriggerRate{true, 1, 1, 1, 1, 500.0f};  // Beat: a bar of 4/4 by default
  bool hold = false;
  ArpUnit unit = ArpUnit::Degrees;
  ArpTranspose transpose = ArpTranspose::Shift;
  int32_t distance = 1;               // −64 … 64, in the unit (Degrees: degrees; Ratio: powers of p/q; Harmonics:
                                      // harmonics; Rotate: notes)
  int32_t steps = 1;                  // 1 … kArpMaxSteps copies of the pattern per cycle
  int32_t ratioP = 2, ratioQ = 1;     // Ratio: 1 … 64 each
  ArpVelocitySource velocitySource = ArpVelocitySource::AsPlayed;
  float fixedVelocity = 0.8f;         // 0 … 1
  float velocityDecayMs = 0.0f;       // 0 = off
  float velocityTarget = 0.0f;        // 0 … 1
  bool velocityRetrigger = true;
  uint32_t seed = 0;
  // AP.3: pitch.
  int32_t harmonicBase = 8;           // Harmonics: the harmonic of a note without a JI group, 1 … 64
  int32_t walkRange = 7;              // Harmonic Walk: h … h + Range, 0 … kArpMaxWalkRange
  bool walkUnder = false;             // Harmonic Walk: undertones
  ArpWalkOrder walkOrder = ArpWalkOrder::Up;
  // AP.3: glides.
  ArpGlide into = ArpGlide::Off;
  ArpGlide out = ArpGlide::Off;
  float intoOffset = 0.0f;            // −4800 … 4800: in the pitch unit (rounded) or cents (glideCents)
  float outOffset = 0.0f;
  bool glideCents = false;
  bool glideSynced = false;           // Time = glideFraction of the note (else glideMs)
  float glideMs = 50.0f;              // 0 … 10000
  float glideFraction = 0.25f;        // 0 … 1
  RampShape glideShape = RampShape::Linear;
  float glideBend = 0.5f;             // −1 … 1 (Exponential: k = 8 · bend; Overshoot: spring 4 · |bend|)
  // AP.4: pulse mode, layers.
  bool pulse = false;
  ArpPulseSource pulseSource = ArpPulseSource::Harmonic;
  // h_ref: 0 = the lowest h held, else 1 … kArpMaxPulseH. AP.8a (Q?-AP.4-a (8) decided): fixed 8 by default (the
  // Harmonics Base), so releasing the lowest note never re-times every clock; 0 is the opt-in "lowest held".
  int32_t pulseRef = 8;
  ArpPulseCycle pulseCycle = ArpPulseCycle::Base;
  int32_t pulseBars = 1;              // Bars: 1 … kArpMaxPulseBars whole notes
  int32_t proportionLen = 0;          // 0 … kArpMaxProportion (0: Proportion falls back to Order)
  std::array<int32_t, kArpMaxProportion> proportion{4, 5, 6, 7, 8, 9, 10, 11};  // 1 … kArpMaxPulseH each
  int32_t layers = 0;                 // 0 … kArpMaxLayers
  std::array<float, kArpMaxLayers> layerLevel{0.7f, 0.5f, 0.35f};  // 0 … 1
  // AP.5: step lanes, the structural part (on / off, length 1 … kArpMaxLaneSteps); the step values are ArpLaneSteps.
  std::array<bool, kArpLaneCount> laneOn{};
  std::array<int32_t, kArpLaneCount> laneLength{16, 16, 16, 16, 16, 16};
  // AP.8a (review L5): memberwise (ArpCore::process re-sanitizes only when the params change; no byte compare of a
  // padded struct).
  friend bool operator==(const ArpParams&, const ArpParams&) = default;
};

// The runtime params a project param slot can drive (ArpStage::map). Values as floats: enums and ints rounded,
// booleans > 0.5.
enum class ArpParamId : uint16_t {
  Style, Synced, RateNum, RateDen, TupletN, TupletM, RateMs, Gate, Groove, Offset, Repeats, Retrigger, RetriggerNum,
  RetriggerDen, Hold, Unit, Transpose, Distance, Steps, RatioP, RatioQ, VelocitySource, FixedVelocity, VelocityDecayMs,
  VelocityTarget, VelocityRetrigger, Seed,
  // AP.3
  HarmonicBase, WalkRange, WalkUnder, WalkOrder, IntoSource, OutTarget, IntoOffset, OutOffset, GlideCents, GlideSynced,
  GlideMs, GlideFraction, GlideShape, GlideBend,
  // AP.4
  Pulse, PulseSource, PulseRef, PulseCycle, PulseBars, ProportionLen, Proportion1, Proportion2, Proportion3,
  Proportion4, Proportion5, Proportion6, Proportion7, Proportion8, Layers, LayerLevel1, LayerLevel2, LayerLevel3,
  // AP.5: per lane (ArpLane order) its switch, its length, then its kArpMaxLaneSteps step values (lane-major).
  VelocityLane, GateLane, GlideLane, TransposeLane, ChanceLane, SkipTieLane,
  VelocityLength, GateLength, GlideLength, TransposeLength, ChanceLength, SkipTieLength,
  LaneStep0,
  LaneStepEnd = LaneStep0 + kArpLaneCount * kArpMaxLaneSteps - 1,
  Count
};
// setArpParam ignores lane step ids (their values are ArpLaneSteps: setArpLaneStep).
void setArpParam(ArpParams& p, ArpParamId id, float value) noexcept;  // RT-safe

// AP.5: the lane step ids.
constexpr bool isArpLaneStep(ArpParamId id) noexcept {
  return static_cast<uint32_t>(id) >= static_cast<uint32_t>(ArpParamId::LaneStep0) &&
         static_cast<uint32_t>(id) <= static_cast<uint32_t>(ArpParamId::LaneStepEnd);
}
constexpr ArpParamId arpLaneStepId(ArpLane lane, int32_t step) noexcept {
  return static_cast<ArpParamId>(static_cast<uint32_t>(ArpParamId::LaneStep0) +
                                 static_cast<uint32_t>(lane) * kArpMaxLaneSteps + static_cast<uint32_t>(step));
}
// The step values of every lane, in each lane's own units (arp.h "Step lanes"); unset: the lane's default.
struct ArpLaneSteps {
  std::array<std::array<float, kArpMaxLaneSteps>, kArpLaneCount> value;
  ArpLaneSteps() noexcept;
};
// A lane's value range and default (the same as its kArpParamInfo rows).
struct ArpLaneRange {
  float min, max, def;
};
inline constexpr ArpLaneRange kArpLaneRange[kArpLaneCount] = {
    {0.0f, 1.0f, 1.0f},     // Velocity: × the velocity (source Lane: the velocity)
    {0.0f, 2.0f, 1.0f},     // Gate: × Gate (the step's gate, clamped 0.01 … 2)
    {0.0f, 1.0f, 1.0f},     // Glide: × the Into / Out glide's pitch distance
    {-48.0f, 48.0f, 0.0f},  // Transpose: + steps in the pitch unit (integers)
    {0.0f, 1.0f, 1.0f},     // Chance: the probability the step plays
    {0.0f, 2.0f, 0.0f},     // Skip / Tie: ArpStepKind
};
// Sets lane step `id` of `s` (false: not a lane step id). RT-safe.
bool setArpLaneStep(ArpLaneSteps& s, ArpParamId id, float value) noexcept;

// AP.2: the ONE declaration of the arpeggiator's params (docs/plans/AP.md "Tasks" AP.2), one row per ArpParamId in enum
// order. The document reads it (pq/document/note_devices.h: the device's params, their ranges / defaults / labels, and
// the settings), so a new ArpParamId needs a row here and nothing in the document. Values are what setArpParam takes:
//   min … max, default   in the param's own units (ms, a fraction of a step, a count; an enum's index; a bool 0 / 1)
//   step                 0: continuous; 1: integers (enums, counts)
//   choices              an enum's labels, '|'-separated, index = value ("" : none)
//   runtime              true: a runtime param (a project param slot, read at every block start: automation and knob
//                        drags never rebuild the chain); false: a structural setting (a change republishes the chain)
struct ArpParamInfo {
  ArpParamId id;
  std::string_view name;   // the document's param / setting name (an identifier)
  std::string_view label;  // shown
  float min, max, def;
  float step;
  bool isBool;
  std::string_view choices;
  bool runtime;
};
inline constexpr ArpParamInfo kArpParamInfo[] = {
    {ArpParamId::Style, "style", "Style", 0, static_cast<float>(ArpStyle::Count) - 1, 0, 1, false,
     "Up|Down|UpDown|DownUp|Up & Down|Down & Up|Converge|Diverge|Con & Diverge|Pinky Up|Pinky UpDown|Thumb Up|"
     "Thumb UpDown|Play Order|Chord Trigger|Random|Random Other|Random Once|Harmonic Walk",
     true},
    {ArpParamId::Synced, "sync", "Sync", 0, 1, 1, 1, true, "", true},
    {ArpParamId::RateNum, "rateNum", "Rate Num", 1, 16, 1, 1, false, "", true},
    {ArpParamId::RateDen, "rateDen", "Rate Den", 1, kArpMaxRateDen, 16, 1, false, "", true},
    {ArpParamId::TupletN, "tupletN", "Tuplet N", 1, kArpMaxTuplet, 1, 1, false, "", true},
    {ArpParamId::TupletM, "tupletM", "Tuplet M", 1, kArpMaxTuplet, 1, 1, false, "", true},
    {ArpParamId::RateMs, "rateMs", "Rate ms", 1, 10000, 125, 0, false, "", true},
    {ArpParamId::Gate, "gate", "Gate", 0.01f, 2, 0.5f, 0, false, "", true},
    {ArpParamId::Groove, "groove", "Groove", 0, 1, 0, 0, false, "", true},
    {ArpParamId::Offset, "offset", "Offset", -32, 32, 0, 1, false, "", true},
    {ArpParamId::Repeats, "repeats", "Repeats", 0, 64, 0, 1, false, "", true},
    {ArpParamId::Retrigger, "retrigger", "Retrigger", 0, 2, 0, 1, false, "Off|Note|Beat", true},
    {ArpParamId::RetriggerNum, "retrigNum", "Retrig Num", 1, 16, 1, 1, false, "", true},
    {ArpParamId::RetriggerDen, "retrigDen", "Retrig Den", 1, kArpMaxRateDen, 1, 1, false, "", true},
    {ArpParamId::Hold, "hold", "Hold", 0, 1, 0, 1, true, "", true},
    {ArpParamId::Unit, "unit", "Unit", 0, 2, 0, 1, false, "Degrees|Ratio|Harmonics", true},
    {ArpParamId::Transpose, "transpose", "Transpose", 0, 1, 0, 1, false, "Shift|Rotate", true},
    {ArpParamId::Distance, "distance", "Distance", -64, 64, 1, 1, false, "", true},
    {ArpParamId::Steps, "steps", "Steps", 1, kArpMaxSteps, 1, 1, false, "", true},
    {ArpParamId::RatioP, "ratioP", "Ratio P", 1, 64, 2, 1, false, "", true},
    {ArpParamId::RatioQ, "ratioQ", "Ratio Q", 1, 64, 1, 1, false, "", true},
    {ArpParamId::VelocitySource, "velSource", "Velocity", 0, 2, 0, 1, false, "As Played|Fixed|Step Lane", true},
    {ArpParamId::FixedVelocity, "velFixed", "Fixed Vel", 0, 1, 0.8f, 0, false, "", true},
    {ArpParamId::VelocityDecayMs, "velDecay", "Vel Decay ms", 0, 10000, 0, 0, false, "", true},
    {ArpParamId::VelocityTarget, "velTarget", "Vel Target", 0, 1, 0, 0, false, "", true},
    {ArpParamId::VelocityRetrigger, "velRetrig", "Vel Retrig", 0, 1, 1, 1, true, "", true},
    {ArpParamId::Seed, "seed", "Seed", 0, 1 << 24, 0, 1, false, "", false},
    {ArpParamId::HarmonicBase, "harmonicBase", "Harm Base", 1, 64, 8, 1, false, "", true},
    {ArpParamId::WalkRange, "walkRange", "Walk Range", 0, kArpMaxWalkRange, 7, 1, false, "", true},
    {ArpParamId::WalkUnder, "walkUnder", "Under", 0, 1, 0, 1, true, "", true},
    {ArpParamId::WalkOrder, "walkOrder", "Walk Order", 0, 3, 0, 1, false, "Up|Down|UpDown|Random", true},
    {ArpParamId::IntoSource, "intoSource", "Into", 0, 2, 0, 1, false, "Off|Step|Offset", true},
    {ArpParamId::OutTarget, "outTarget", "Out", 0, 2, 0, 1, false, "Off|Step|Offset", true},
    {ArpParamId::IntoOffset, "intoOffset", "Into Offset", -4800, 4800, 0, 0, false, "", true},
    {ArpParamId::OutOffset, "outOffset", "Out Offset", -4800, 4800, 0, 0, false, "", true},
    {ArpParamId::GlideCents, "glideCents", "Cents", 0, 1, 0, 1, true, "", true},
    {ArpParamId::GlideSynced, "glideSync", "Glide Sync", 0, 1, 0, 1, true, "", true},
    {ArpParamId::GlideMs, "glideMs", "Glide ms", 0, 10000, 50, 0, false, "", true},
    {ArpParamId::GlideFraction, "glideFraction", "Glide Frac", 0, 1, 0.25f, 0, false, "", true},
    {ArpParamId::GlideShape, "glideShape", "Shape", 0, static_cast<float>(RampShape::Count) - 1, 0, 1, false,
     "Linear|Exponential|S-Curve|Overshoot", true},
    {ArpParamId::GlideBend, "glideBend", "Bend", -1, 1, 0.5f, 0, false, "", true},
    {ArpParamId::Pulse, "pulse", "Pulse", 0, 1, 0, 1, true, "", true},
    {ArpParamId::PulseSource, "pulseSource", "Pulse h", 0, 2, 0, 1, false, "Harmonic|Proportion|Order", true},
    {ArpParamId::PulseRef, "pulseRef", "Pulse Ref", 0, kArpMaxPulseH, 8, 1, false, "", true},
    {ArpParamId::PulseCycle, "pulseCycle", "Cycle", 0, 1, 0, 1, false, "Base|Bars", true},
    {ArpParamId::PulseBars, "pulseBars", "Bars", 1, kArpMaxPulseBars, 1, 1, false, "", true},
    {ArpParamId::ProportionLen, "proportionLen", "Prop Len", 0, kArpMaxProportion, 0, 1, false, "", true},
    {ArpParamId::Proportion1, "proportion1", "Prop 1", 1, kArpMaxPulseH, 4, 1, false, "", true},
    {ArpParamId::Proportion2, "proportion2", "Prop 2", 1, kArpMaxPulseH, 5, 1, false, "", true},
    {ArpParamId::Proportion3, "proportion3", "Prop 3", 1, kArpMaxPulseH, 6, 1, false, "", true},
    {ArpParamId::Proportion4, "proportion4", "Prop 4", 1, kArpMaxPulseH, 7, 1, false, "", true},
    {ArpParamId::Proportion5, "proportion5", "Prop 5", 1, kArpMaxPulseH, 8, 1, false, "", true},
    {ArpParamId::Proportion6, "proportion6", "Prop 6", 1, kArpMaxPulseH, 9, 1, false, "", true},
    {ArpParamId::Proportion7, "proportion7", "Prop 7", 1, kArpMaxPulseH, 10, 1, false, "", true},
    {ArpParamId::Proportion8, "proportion8", "Prop 8", 1, kArpMaxPulseH, 11, 1, false, "", true},
    {ArpParamId::Layers, "layers", "Layers", 0, kArpMaxLayers, 0, 1, false, "", true},
    {ArpParamId::LayerLevel1, "layerLevel1", "Layer 1", 0, 1, 0.7f, 0, false, "", true},
    {ArpParamId::LayerLevel2, "layerLevel2", "Layer 2", 0, 1, 0.5f, 0, false, "", true},
    {ArpParamId::LayerLevel3, "layerLevel3", "Layer 3", 0, 1, 0.35f, 0, false, "", true},
    // AP.5: lane switches and lengths are structural; the steps are runtime params.
    {ArpParamId::VelocityLane, "velLane", "Vel Lane", 0, 1, 0, 1, true, "", false},
    {ArpParamId::GateLane, "gateLane", "Gate Lane", 0, 1, 0, 1, true, "", false},
    {ArpParamId::GlideLane, "glideLane", "Glide Lane", 0, 1, 0, 1, true, "", false},
    {ArpParamId::TransposeLane, "transLane", "Trans Lane", 0, 1, 0, 1, true, "", false},
    {ArpParamId::ChanceLane, "chanceLane", "Chance Lane", 0, 1, 0, 1, true, "", false},
    {ArpParamId::SkipTieLane, "tieLane", "Skip/Tie Lane", 0, 1, 0, 1, true, "", false},
    {ArpParamId::VelocityLength, "velLen", "Vel Len", 1, kArpMaxLaneSteps, 16, 1, false, "", false},
    {ArpParamId::GateLength, "gateLen", "Gate Len", 1, kArpMaxLaneSteps, 16, 1, false, "", false},
    {ArpParamId::GlideLength, "glideLen", "Glide Len", 1, kArpMaxLaneSteps, 16, 1, false, "", false},
    {ArpParamId::TransposeLength, "transLen", "Trans Len", 1, kArpMaxLaneSteps, 16, 1, false, "", false},
    {ArpParamId::ChanceLength, "chanceLen", "Chance Len", 1, kArpMaxLaneSteps, 16, 1, false, "", false},
    {ArpParamId::SkipTieLength, "tieLen", "Skip/Tie Len", 1, kArpMaxLaneSteps, 16, 1, false, "", false},
#define PQ_ARP_STEPS(M, ...)                                                                                          M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__)         M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) M(11, __VA_ARGS__) M(12, __VA_ARGS__)      M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__) M(16, __VA_ARGS__) M(17, __VA_ARGS__) M(18, __VA_ARGS__)   M(19, __VA_ARGS__) M(20, __VA_ARGS__) M(21, __VA_ARGS__) M(22, __VA_ARGS__) M(23, __VA_ARGS__) M(24, __VA_ARGS__)   M(25, __VA_ARGS__) M(26, __VA_ARGS__) M(27, __VA_ARGS__) M(28, __VA_ARGS__) M(29, __VA_ARGS__) M(30, __VA_ARGS__)   M(31, __VA_ARGS__) M(32, __VA_ARGS__)
#define PQ_ARP_STEP_ROW(n, lane, name, label, step, choices)                                                         {arpLaneStepId(ArpLane::lane, n - 1), name #n, label " " #n, kArpLaneRange[static_cast<size_t>(ArpLane::lane)].min,    kArpLaneRange[static_cast<size_t>(ArpLane::lane)].max, kArpLaneRange[static_cast<size_t>(ArpLane::lane)].def,       step, false, choices, true},
    PQ_ARP_STEPS(PQ_ARP_STEP_ROW, Velocity, "velStep", "Vel", 0, "")
    PQ_ARP_STEPS(PQ_ARP_STEP_ROW, Gate, "gateStep", "Gate", 0, "")
    PQ_ARP_STEPS(PQ_ARP_STEP_ROW, Glide, "glideStep", "Glide", 0, "")
    PQ_ARP_STEPS(PQ_ARP_STEP_ROW, Transpose, "transStep", "Trans", 1, "")
    PQ_ARP_STEPS(PQ_ARP_STEP_ROW, Chance, "chanceStep", "Chance", 0, "")
    PQ_ARP_STEPS(PQ_ARP_STEP_ROW, SkipTie, "tieStep", "Step", 1, "Play|Skip|Tie")
#undef PQ_ARP_STEP_ROW
#undef PQ_ARP_STEPS
};
static_assert(std::size(kArpParamInfo) == static_cast<size_t>(ArpParamId::Count),
              "kArpParamInfo: one row per ArpParamId");
consteval bool arpParamInfoInOrder() {
  for (size_t i = 0; i < std::size(kArpParamInfo); ++i)
    if (static_cast<size_t>(kArpParamInfo[i].id) != i) return false;
  return true;
}
static_assert(arpParamInfoInOrder(), "kArpParamInfo: rows in ArpParamId order");
// An enum's labels: one per value of its range.
consteval bool arpChoicesMatchRanges() {
  for (const ArpParamInfo& r : kArpParamInfo) {
    if (r.choices.empty()) continue;
    size_t n = 1;
    for (const char c : r.choices) n += c == '|' ? 1 : 0;
    if (static_cast<float>(n) != r.max - r.min + 1) return false;
  }
  return true;
}
static_assert(arpChoicesMatchRanges(), "kArpParamInfo: an enum's choices are one per value of its range");
inline const ArpParamInfo& arpParamInfo(ArpParamId id) noexcept { return kArpParamInfo[static_cast<size_t>(id)]; }
ArpParams sanitized(const ArpParams& p) noexcept;                       // clamps every field to its range. RT-safe

// Degrees: the tuning's degrees in cents within one equave and the equave in cents. AP.8a: cents are measured from
// the reference (cents[0] = degree 0's, 0 when the reference sits on degree 0), and `referenceHz` makes the table an
// absolute degree → Hz map: degree D sounds referenceHz · 2^(cents(D) / 1200) (D = equave · N + degree, cents(D) =
// q · equave + cents[D − q · N]) — what a tuning edit re-resolves a held note with (ArpTables). referenceHz 0: unknown
// (the table's intervals still shift degrees; a tuning edit then leaves the held notes' Hz).
struct ArpDegreeTable {
  std::vector<double> cents;  // empty: 12-EDO
  double equaveCents = 1200.0;
  double referenceHz = 0.0;
  friend bool operator==(const ArpDegreeTable&, const ArpDegreeTable&) = default;
};

// AP.8a (review H4): the degree tables of a stage, one per tuning its notes follow. A note event's `tuning` (voice.tuning:
// the track patch's tuning node it follows, 1-based; note_events.h) picks nodes[tuning − 1]; a note without one (0: a
// static pitch — private, Scala, MODMOS tunings, or a track without a tuning node) plays the track's table. So a
// clip with its own tuning steps in ITS degrees, and a tuning edit re-resolves each note in its own tuning.
struct ArpTables {
  ArpDegreeTable track;
  std::vector<ArpDegreeTable> nodes;
  ArpTables() = default;
  ArpTables(ArpDegreeTable t) : track(std::move(t)) {}  // NOLINT: one table for every note
  const ArpDegreeTable& of(float tuning) const noexcept {  // RT-safe
    return tuning >= 1.0f && tuning <= static_cast<float>(nodes.size()) ? nodes[static_cast<size_t>(tuning) - 1] : track;
  }
  friend bool operator==(const ArpTables&, const ArpTables&) = default;
};

struct ArpStats {
  uint64_t steps = 0;           // steps played (a chord trigger counts one)
  uint64_t notes = 0;           // generated note-ons
  uint64_t droppedHeld = 0;     // source note-ons beyond kArpMaxHeld
  uint64_t cutSounding = 0;     // generated notes ended early beyond kArpMaxSounding
  uint64_t transportJumps = 0;  // loop wraps / locates / stops seen while busy
  uint64_t glides = 0;          // Into / Out glides sent (AP.3)
  uint64_t droppedWalk = 0;     // Harmonic Walk elements beyond kArpMaxWalk (per rebuild)
  uint64_t pulses = 0;          // AP.4: pulse-clock onsets played (a pulse with layers counts one)
  uint64_t ties = 0;            // AP.5: notes a tie step continued
  uint64_t laneRests = 0;       // AP.5: steps / pulses a Skip or a failed Chance silenced
  uint64_t tempoRemaps = 0;     // AP.8a: tempo swaps under a running song clock (StageClock::remapped: not a locate)
};

// AP.2b: new degree tables for a running stage (a tuning edit: no republish, the pattern keeps its place). One
// publisher (non-RT: post, collect) and the stage's audio thread (take, at a block start). Lock-free: the audio thread
// keeps the table it played last block valid through the block it takes a new one (the arp re-resolves its held notes
// from the old to the new one) and retires it at its next take into a free retired slot; post and collect free the
// retired ones (nothing is freed on the audio thread, a table is never freed while it reads it). AP.8a (review M1): the
// retired tables are kRetiredSlots slots, so a post racing a take can never leave a newer table untaken until a later
// edit; ProjectLink::collect also frees them, so the slots never fill while edits stop.
class ArpTableMailbox {
public:
  static constexpr uint32_t kRetiredSlots = 4;
  ArpTableMailbox() = default;
  ArpTableMailbox(const ArpTableMailbox&) = delete;
  ArpTableMailbox& operator=(const ArpTableMailbox&) = delete;
  ~ArpTableMailbox();
  void post(ArpTables tables);           // non-RT (allocates, frees the retired / an untaken table set)
  void collect();                        // non-RT: frees the retired table sets
  // RT-safe: the newest posted table set (null: none, or no free retired slot: next block). The one it replaces stays
  // valid until the next take.
  const ArpTables* take() noexcept;
  bool pending() const noexcept { return pending_.load(std::memory_order_acquire) != nullptr; }

private:
  std::atomic<ArpTables*> pending_{nullptr};
  std::array<std::atomic<ArpTables*>, kRetiredSlots> retired_{};
  ArpTables* current_ = nullptr;  // audio thread: the one played
  ArpTables* prev_ = nullptr;     // audio thread: the one it replaced (read in the block of the take)
};

// AP.7 (docs/plans/AP.md "Roll preview + Flatten"): what a generated note plays relative to its source note, EXACTLY
// — the same rules as its Hz (Pitch units, Harmonic Walk, Layers), kept as integers: the roll preview and Flatten give
// it the document identity the source's identity times this (pq/app/note_stage_render.h).
//   Same      the source's identity unchanged
//   Degrees   + `degrees` degrees of the stage's table (Degrees unit, no walk / layer): exact for a source with a
//             Degree identity (the arp shifts from its degree, or from round(noteDegree) when the event has none)
//   Ratio     × num / den (reduced, both > 0): Harmonics / Ratio units, Harmonic Walk, Layers
//   Inexact   neither (a degree shift on a walked or layered note, a ratio too large to keep exact): only its Hz
struct ArpExact {
  enum class Kind : uint8_t { Same, Degrees, Ratio, Inexact };
  Kind kind = Kind::Same;
  int64_t degrees = 0;
  int64_t num = 1, den = 1;
};
// One generated note-on (its id in the stage's id space) and the held source note it plays (the source's event id).
struct ArpTraceNote {
  uint32_t id = 0, source = 0;
  ArpExact exact;
};

class ArpCore {
public:
  // Non-RT (allocates every table). `stageIndex`: the id space (note_stage.h stageNoteId).
  explicit ArpCore(uint32_t stageIndex = 0, ArpTables tables = {});
  ArpCore(const ArpCore&) = delete;
  ArpCore& operator=(const ArpCore&) = delete;
  ~ArpCore();

  void setStageIndex(uint32_t index) noexcept { stageIndex_ = index; }  // non-RT (before the first block)
  // One block: `in` (sorted by frame) → `out` (pushed in frame order). Returns true while busy. RT-safe.
  bool process(const ArpParams& params, const NoteStageBlock& block, const EventList& in, EventBuffer& out) noexcept;
  // Handover (note_stage.h): offs for the sounding generated notes, the key-held source notes as note-ons. RT-safe.
  void flush(uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;
  // AP.2b: takes over `old`'s running state (held set, clock, pattern / lane positions, sounding notes, RNG
  // counters), keeping its own structure (degree table, its mailbox, lane values / slots, id space); `old` is left
  // idle. Its params apply from the next block. RT-safe (swaps, no allocation).
  void adopt(ArpCore& old) noexcept;

  // AP.5: the lane step values: fixed ones, and per step an optional project param slot read when the step plays
  // (value = offset + scale · params[slot]; arp.h "Step lanes"). Non-RT (before the first block, or while no block
  // runs: tests and offline runs set fixed values between blocks).
  void setLaneSteps(const ArpLaneSteps& steps) noexcept;
  void mapLaneStep(ArpLane lane, int32_t step, uint32_t param, float scale, float offset) noexcept;
  // AP.8b (review M8): the cables into a lane step — bus values (NoteStageBlock::mods, in the lane's units) added to
  // the step's value when it plays (read only then: no per-block cost). Once per step (allocates). Non-RT.
  void modLaneStep(ArpLane lane, int32_t step, std::span<const uint32_t> buses);
  // AP.2b: where new degree tables come from while the core runs (non-RT, before the first block).
  void setTableMailbox(std::shared_ptr<ArpTableMailbox> box) noexcept;
  // AP.6: where the core shows its device panel what it plays (pq/core/note_stage_status.h): the lane positions and
  // pattern position at every step, the pattern list when the held set or its order changes, idle when nothing is held.
  // Null (the default): nothing is written. Non-RT (before the first block); kept by adopt (each core keeps its own).
  void setStatus(std::shared_ptr<NoteStageStatus> status) noexcept;
  // AP.7: OFFLINE ONLY (the roll preview, Flatten; the audio thread never sets one): every generated note-on appends
  // its ArpTraceNote to `trace` (allocates). Null (the default): nothing is recorded (one branch per note-on).
  void setTrace(std::vector<ArpTraceNote>* trace) noexcept;

  // AP.8a (review L4): starts the generated-id counter at `n` (tests: the 2^24 wrap). Non-RT.
  void setNoteCounter(uint32_t n) noexcept;

  uint32_t heldCount() const noexcept;      // RT-safe
  bool idle() const noexcept;               // RT-safe: nothing held, nothing sounding
  uint32_t soundingCount() const noexcept;  // RT-safe
  const ArpStats& stats() const noexcept;

private:
  struct State;
  std::unique_ptr<State> s_;
  uint32_t stageIndex_;
};

// The arpeggiator as a note stage. `params` is the fixed part; `map` lists project param slots read at every block
// start into the listed fields (AP.2's lowering fills it: runtime params, automatable). Non-RT to build.
class ArpStage final : public NoteStage {
public:
  struct Mapping {
    ArpParamId id;
    uint32_t param;  // index into NoteStageBlock::params (out of range: ignored)
    // AP.2: the slot's value v gives the field offset + scale · v (setArpParam rounds / clamps). The document's slots
    // hold knob POSITIONS in [0, 1] (what automation writes): scale = max − min, offset = min of the param's linear
    // curve. Default: the slot holds the value itself.
    float scale = 1.0f, offset = 0.0f;
    bool add = false;  // AP.2b: adds NoteStageBlock::mods[param] (a bus value; scale / offset unused) to the id's value
  };
  // Mappings of lane step ids (isArpLaneStep) go to the core's lane table (read only when their step plays, not at
  // every block start; AP.8b: their add mappings too, ArpCore::modLaneStep). AP.2b: Mapping::add entries add a bus value (in the param's units: a modulation wire's
  // previous block, lower.h "Note devices") to the mapping of the same id before them; `steps` the fixed lane values;
  // `box` (optional) hands it later degree tables.
  // AP.6: `status` (optional) is what the device panel reads (ArpCore::setStatus).
  explicit ArpStage(const ArpParams& params = {}, std::vector<Mapping> map = {}, ArpTables tables = {},
                    const ArpLaneSteps& steps = {}, std::shared_ptr<ArpTableMailbox> box = {},
                    std::shared_ptr<NoteStageStatus> status = {});
  void setIndex(uint32_t index) override { core_.setStageIndex(index); }
  ArpCore& core() noexcept { return core_; }
  const ArpParams& params() const noexcept { return params_; }

private:
  static bool processStage(void* self, const NoteStageBlock& block, const EventList& in, EventBuffer& out) noexcept;
  static void flushStage(void* self, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;
  static void adoptStage(void* self, void* old) noexcept;
  ArpParams params_;
  std::vector<Mapping> map_;
  ArpCore core_;
};

}  // namespace pq::engine
