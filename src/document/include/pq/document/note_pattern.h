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

// AP.10: the GLIDES of one step of note device `device` as its bend view draws them (its values now, at `bpm`): the
// step's length, the note's (Gate), and each side's mode, offset (its param value, in cents, as text in the unit), time
// (AP.11: a fraction 0 … 2 of the note → steps / ms; the sides are additive and may overlap or pass the note: the
// engine's rule, arp.h "Glides"), shape and bend.
struct NoteGlideSide {
  int32_t mode = 0;            // 0 Off, 1 Step (Into: the previous step, Out: the next one), 2 Offset
  double offset = 0.0;         // the Offset param's value (in the unit, or cents)
  double cents = 0.0;          // the offset in cents (NaN: a rest — Harmonics below the first)
  std::string text;            // the offset as the unit reads it: "−2 deg", "+100 ¢", "h8 → h9", "× (3/2)^1"
  double time = 0.0;           // its Time, a fraction of the note (0: Off)
  double steps = 0.0, ms = 0.0;  // the same in steps / ms
  int32_t shape = 0;           // RampShape
  double bend = 0.0;           // −1 … 1
};
struct NoteGlideView {
  bool valid = false;
  double stepMs = 0.0;         // one step (a Rate period)
  double gate = 0.5;           // the note's length in steps
  bool cents = false;          // Offsets in cents
  NoteGlideSide into, out;
};
NoteGlideView noteGlideView(const Project& p, DeviceId device, double bpm = 120.0);
// The cents an Into / Out offset of `offset` moves the note in the device's unit now (Glide cents: itself; Degrees: the
// track table's interval from degree 0; Harmonics: Base → Base + k; Ratio: (p/q)^k). NaN: a rest.
double noteGlideOffsetCents(const Project& p, DeviceId device, double offset);

// The voice's glide ramp s(u) (pq/engine/note_events.h rampShapeAt) of RampShape `shape` (0 Linear, 1 Exponential,
// 2 S-curve, 3 Overshoot) with an arpeggiator Bend −1 … 1: what the panel's drawings evaluate.
double noteRampShape(int32_t shape, double bend, double u) noexcept;

}  // namespace pq::doc
