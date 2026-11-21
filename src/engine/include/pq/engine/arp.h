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
// unchanged; AllNotesOff / AllSoundOff pass through and clear the held set, the generated notes and the clock.
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
// stop drops the latched notes no key holds.
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
// Degrees: degree D + k of the note's tuning (D = equave · N + degree; the new degree / equave normalized, `tuning`
// kept, so a patch's tuning node still drives the voice: T2.6). The Hz moves by cents(D + k) − cents(D) of the
// stage's ArpDegreeTable (the track tuning's step table, set off-thread with the stage; default 12-EDO). A note
// without a Degree identity is shifted by the table's interval from degree round(noteDegree) (0 when unknown) and
// plays static (tuning 0) when shifted. Under a modulated tuning (a track patch's tuning_hz node, T2.6) the voice
// derives its Hz from the event's degree / equave per sample, and the Hz the table gives is that degree's static
// pitch (the node's E = hz / hz₀ = 1), so the step follows the modulation and an arp glide composes on top.
// Ratio: × (p/q)^e (e = j · Distance, or the Rotate wraps), an exact integer fraction (Hz = hz · num / den in double,
// float32 at the end): ratio = source ratio · num/den, harmonic = h · num/den when that is an integer (else 0),
// degree / equave NaN, tuning 0 (a static pitch), noteDegree moved by the table's steps (fractional).
// Harmonics (AP.3): a note with voice.harmonic h > 0 (an integer: its number in its JI group, anchor = Hz · h_a / h)
// steps to h + k over the same anchor; a note without one counts as harmonic Base of itself. Hz = hz · (h + k) / h
// (double, float32 at the end), ratio = source ratio · (h + k) / h, harmonic = h + k exactly; degree / equave NaN,
// tuning 0 (static), noteDegree moved fractionally. h + k < 1 is a rest (no note). Rotate's wrap is an octave of
// the series (h → 2h).
// A copy with no shift carries the source's identity unchanged.
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
// ---- Velocity, expression -----------------------------------------------------------------------------------------
// Velocity: as played (the source note's) or Fixed; Decay (ms, 0 = off) moves it linearly to Target from the pattern
// start (and from every retrigger when Velocity Retrigger is on). Expression carry-over: a Pitch / Gain event of a
// held source note applies to every step that note is producing — sounding ones get the event (Pitch: their Hz
// times the source's new bend, same ramp; Gain: the same gain and ramp) and later steps start at the source's
// current bend / gain (inside a ramp: its value at the step, then the rest of the ramp). A retune (kNoteEventRetune)
// re-derives the held note and its sounding steps' identities. Sync / conditional gains of a held note are dropped.
//
// ---- Capacities -----------------------------------------------------------------------------------------------------
// kArpMaxHeld source notes (a note-on beyond: dropped, counted); kArpMaxSounding generated notes (beyond: the oldest
// ends at the new step's frame, counted). Per block O(events + steps + gate ends in the block); a block with nothing
// held, nothing sounding and no event returns after one compare.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

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
enum class ArpVelocitySource : uint8_t { AsPlayed, Fixed };

// A step length: Synced num/den of a whole note × m/n (n notes in the time of m), or Free `ms`.
struct ArpRate {
  bool synced = true;
  int32_t num = 1, den = 16;          // 1 … 16 / 1 … kArpMaxRateDen
  int32_t tupletN = 1, tupletM = 1;   // 1 … kArpMaxTuplet: straight 1:1, triplet 3:2, dotted 2:3, quintuplet 5:4
  float ms = 125.0f;                  // Free: 1 … 10000 ms
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
};

// The runtime params a project param slot can drive (ArpStage::map). Values as floats: enums and ints rounded,
// booleans > 0.5.
enum class ArpParamId : uint8_t {
  Style, Synced, RateNum, RateDen, TupletN, TupletM, RateMs, Gate, Groove, Offset, Repeats, Retrigger, RetriggerNum,
  RetriggerDen, Hold, Unit, Transpose, Distance, Steps, RatioP, RatioQ, VelocitySource, FixedVelocity, VelocityDecayMs,
  VelocityTarget, VelocityRetrigger, Seed,
  // AP.3
  HarmonicBase, WalkRange, WalkUnder, WalkOrder, IntoSource, OutTarget, IntoOffset, OutOffset, GlideCents, GlideSynced,
  GlideMs, GlideFraction, GlideShape, GlideBend,
  Count
};
void setArpParam(ArpParams& p, ArpParamId id, float value) noexcept;  // RT-safe
ArpParams sanitized(const ArpParams& p) noexcept;                       // clamps every field to its range. RT-safe

// Degrees: the tuning's degrees in cents within one equave (cents[0] = 0, size N ≥ 1) and the equave in cents.
struct ArpDegreeTable {
  std::vector<double> cents;  // empty: 12-EDO
  double equaveCents = 1200.0;
};

struct ArpStats {
  uint64_t steps = 0;           // steps played (a chord trigger counts one)
  uint64_t notes = 0;           // generated note-ons
  uint64_t droppedHeld = 0;     // source note-ons beyond kArpMaxHeld
  uint64_t cutSounding = 0;     // generated notes ended early beyond kArpMaxSounding
  uint64_t transportJumps = 0;  // loop wraps / locates / stops seen while busy
  uint64_t glides = 0;          // Into / Out glides sent (AP.3)
  uint64_t droppedWalk = 0;     // Harmonic Walk elements beyond kArpMaxWalk (per rebuild)
};

class ArpCore {
public:
  // Non-RT (allocates every table). `stageIndex`: the id space (note_stage.h stageNoteId).
  explicit ArpCore(uint32_t stageIndex = 0, ArpDegreeTable table = {});
  ArpCore(const ArpCore&) = delete;
  ArpCore& operator=(const ArpCore&) = delete;
  ~ArpCore();

  void setStageIndex(uint32_t index) noexcept { stageIndex_ = index; }  // non-RT (before the first block)
  // One block: `in` (sorted by frame) → `out` (pushed in frame order). Returns true while busy. RT-safe.
  bool process(const ArpParams& params, const NoteStageBlock& block, const EventList& in, EventBuffer& out) noexcept;
  // Handover (note_stage.h): offs for the sounding generated notes, the key-held source notes as note-ons. RT-safe.
  void flush(uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;

  uint32_t heldCount() const noexcept;      // RT-safe
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
  };
  explicit ArpStage(const ArpParams& params = {}, std::vector<Mapping> map = {}, ArpDegreeTable table = {});
  void setIndex(uint32_t index) override { core_.setStageIndex(index); }
  ArpCore& core() noexcept { return core_; }
  const ArpParams& params() const noexcept { return params_; }

private:
  static bool processStage(void* self, const NoteStageBlock& block, const EventList& in, EventBuffer& out) noexcept;
  static void flushStage(void* self, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept;
  ArpParams params_;
  std::vector<Mapping> map_;
  ArpCore core_;
};

}  // namespace pq::engine
