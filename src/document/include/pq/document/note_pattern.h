#pragma once

// AP.10 (docs/plans/AP.md "AP.10"): the RESOLVED PATTERN of a note device for its panel — the whole cycle its params
// make of a held set (Distance / Steps transpositions, Harmonic Walk elements, Ratio / Harmonics steps, Layers, Pulse
// clocks, lanes), each note's pitch curve (its Into / Out glides as the voice plays them) and its identity in the
// device's Pitch unit. Computed by the engine's own ArpCore run offline (pq/engine/arp_sketch.h sketchArp: nothing
// re-implemented) over the device's current values and its track's degree tables; the views draw it as plain points
// (they never include the engine). Not RT code; the caller caches it (one run per change of params / held set / tempo).

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pq/document/model.h"

namespace pq::doc {

// One held note's identity (pq/core/note_stage_status.h NoteStageStatus::HeldNote; NaN degree / equave: none).
struct NotePatternHeld {
  float hz = 0.0f, velocity = 0.8f, ratio = 1.0f, harmonic = 0.0f, degree = 0.0f, equave = 0.0f, tuning = 0.0f,
        noteDegree = 0.0f;
};

struct NotePatternNote {
  float on = 0.0f, off = 0.0f;  // steps (Rate periods) from the cycle start
  float hz = 0.0f;              // its own pitch (an Into glide's target)
  bool source = false;          // the held note itself (else generated: a transposition, walk step, layer)
  std::string label;            // its identity in the unit: "+3" degrees, "h11", "3/2" ("": a source in Degrees)
  int32_t position = -1, copy = 0;  // its step's status position / copy (what the live status lights)
  bool bends = false;           // it has Pitch events (a glide)
  std::vector<std::pair<float, float>> curve;  // (steps, log2 Hz) from on to off: its pitch as the voice plays it
};

struct NotePattern {
  bool valid = false;           // the device is a note device the sketch knows
  std::vector<NotePatternNote> notes;
  double cycle = 0.0;           // steps the view spans
  double stepMs = 0.0;
  bool truncated = false;       // the cycle is longer than 64 steps (the first 64)
  bool nominal = false;         // nothing held: a reference note of the track's tuning
  float lo = 0.0f, hi = 0.0f;   // log2 Hz range of every curve
  std::vector<std::pair<float, bool>> grid;  // reference lines in [lo, hi] (log2 Hz, major)
};

// The pattern of note device `device` over `held` (empty: the nominal note) at `bpm`. Library-free.
NotePattern notePattern(const Project& p, DeviceId device, std::span<const NotePatternHeld> held, double bpm = 120.0);

// AR.2 (was AP.10): the GLIDES of one step of note device `device` as its bend view draws and edits them (its values
// now, at `bpm`): the step's length, the note's (Gate), and each side as the engine reads it (pq/engine/arp.h "Glides":
// an offset in cents | the neighbouring step, a time — a fraction 0 … 2 of the note; the sides are additive and may
// overlap or pass the note —, a curve and a shape), plus the offsets a drag snaps to.
struct NoteGlideSide {
  bool on = false;             // it glides (step, or an offset that is not 0)
  bool step = false;           // from the previous step (Into) / to the next step (Out) instead of the offset
  double offset = 0.0;         // cents (the param)
  std::string text;            // what it reads: "−2 deg", "h8 → h9", "× (3/2)^1", "+35 ¢", "prev step", "off"
  double timeParam = 0.0;      // its Time param, a fraction of the note
  double time = 0.0;           // the same while it is on, else 0
  double steps = 0.0, ms = 0.0;  // `time` in steps / ms
  int32_t shape = 0;           // the voice's RampShape (noteRampShape)
  double curve = 0.0;          // −1 … 1
};
struct NoteGlideView {
  bool valid = false;
  double stepMs = 0.0;         // one step (a Rate period)
  double gate = 0.5;           // the note's length in steps
  double maxOffset = 2400.0;   // an offset's range, cents either way (the engine's kArpMaxGlideOffset)
  bool laneSlide = false;      // the Glide lane is on with no side on: its painted steps slide in from the previous step
  NoteGlideSide into, out;
  // The offsets (cents, ascending, 0 among them, within the offset's range) a drag snaps to: the device's pitch unit's
  // steps of the NOMINAL note (Degrees: the track table's intervals from degree 0; Harmonics: Base → Base + k; Ratio:
  // (p/q)^k).
  std::vector<double> snaps;
};
NoteGlideView noteGlideView(const Project& p, DeviceId device, double bpm = 120.0);
// The snap of `v` nearest `cents` (itself when there is none).
double noteGlideSnap(const NoteGlideView& v, double cents) noexcept;

// The voice's glide ramp s(u) (pq/engine/note_events.h rampShapeAt) of RampShape `shape` (0 Linear, 1 Exponential,
// 2 S-curve, 3 Overshoot) with an arpeggiator Bend −1 … 1: what the panel's drawings evaluate.
double noteRampShape(int32_t shape, double bend, double u) noexcept;

// AR.4: what a transpose lane rules its Degrees grid by — the tuning note device `device`'s track steps in (null: none;
// pointer walking only: cheap enough per paint) and the degrees in one equave of a tuning's degree table (12: none /
// no table; builds the table: cache it by the tuning).
const TuningDef* noteDeviceTuning(const Project& p, DeviceId device) noexcept;
int32_t tuningEquaveDegrees(const TuningDef* def);

// AR.3 (pq/engine/arp.h "Pulse mode"): the PULSE ROWS of note device `device` as its pulse view draws and edits them:
// the Cycle (num / den bars) and, per row from the lowest held note up, the division the engine clocks it at (Auto: the
// note's JI harmonic — `locked`: the row's Division param does not apply —, else the row's Division), its 32-bit mask
// (pulse j plays when bit j mod 32 is set) and its shift (of its period). Rows = the held notes by Hz (the first
// kNotePulseRows; more held: they share the rows again from the first), at least kNotePulseMinRows — a row without a
// held note (`held` false) shows what the next note added would play.
inline constexpr uint32_t kNotePulseRows = 8, kNotePulseMinRows = 4;
struct NotePulseRow {
  int32_t division = 1;
  bool locked = false;
  bool held = false;
  uint32_t mask = 0xFFFFFFFFu;
  double shift = 0.0;
};
struct NotePulseView {
  bool valid = false;       // the device is a note device with pulse rows
  bool on = false;          // Pulse is on
  int32_t cycleNum = 1, cycleDen = 1;
  int32_t maxDivision = 64;
  double maxShift = 0.5;
  std::vector<NotePulseRow> rows;
};
NotePulseView notePulseView(const Project& p, DeviceId device, std::span<const NotePatternHeld> held);
// Euclid k of n as a row mask (the engine's arpEuclidMask: k onsets spread evenly over n pulses, n ≤ 32, repeated over
// the 32 bits), and a mask rotated by `by` pulses within its first n (later / earlier; repeated likewise).
uint32_t notePulseEuclid(int32_t k, int32_t n) noexcept;
uint32_t notePulseRotate(uint32_t mask, int32_t n, int32_t by) noexcept;

}  // namespace pq::doc
