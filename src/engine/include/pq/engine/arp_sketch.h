#pragma once

// AP.10 (docs/plans/AP.md "AP.10"): the RESOLVED CYCLE of an arpeggiator over a held set — what the device panel's
// pattern view and bend view draw. It runs the REAL ArpCore (arp.h: the code the audio thread runs, nothing
// re-implemented) offline: a free clock (the transport stopped) at `sampleRate`, the held notes on at frame 0, blocks of
// at most `blockFrames` until one cycle has played (+ the last notes' ends). Every generated note: its start / end in
// steps (Rate periods) from the cycle start, its pitch events (Into / Out glides, with the shapes the voice plays:
// note_events.h rampShapeAt), its source, its exact identity (ArpExact) and the status position / copy its step wrote
// (what the live status lights). Not RT code (allocates); deterministic: the same inputs give the same sketch.
//
//   cycle       pattern mode: the period × Steps (Chord Trigger: Steps; Random: the n elements × Steps); Pulse: the
//               Cycle in Rate periods (ArpCore::cycleSteps); at most maxSteps (longer: the first maxSteps, truncated).
//   Repeats     a finite Repeats that ends inside the cycle ends the sketch there (nothing more plays).
//   Chance      drawn as playback draws it (seed, grid step index of the free clock).
// Cost: O(steps × notes) events; a typical 16-step cycle is ~600 blocks of a few compares each (well under a ms).

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "pq/engine/arp.h"

namespace pq::engine {

// One held source note (its note-on: identity as a NoteEvent carries it).
struct ArpSketchHeld {
  float hz = 0.0f;
  float velocity = 0.8f;
  float ratio = 1.0f;
  float harmonic = 0.0f;
  float degree = std::numeric_limits<float>::quiet_NaN();
  float equave = std::numeric_limits<float>::quiet_NaN();
  float tuning = 0.0f;
  float noteDegree = std::numeric_limits<float>::quiet_NaN();
};

struct ArpSketchConfig {
  double sampleRate = 4800.0;  // the sketch's resolution (0.2 ms)
  double bpm = 120.0;          // the free clock's tempo (synced rates)
  double beatsPerBar = 4.0;    // the bar a pulse Cycle counts (quarter notes)
  uint32_t maxSteps = 64;
  uint32_t blockFrames = 32;
};

// A Pitch event of a sketched note: at `at` (steps) it moves to `hz` over `len` steps with `shape` / `bend`.
struct ArpSketchBend {
  float at = 0.0f, len = 0.0f;
  float hz = 0.0f;
  RampShape shape = RampShape::Linear;
  int8_t bend = 0;
};

struct ArpSketchNote {
  float on = 0.0f, off = 0.0f;  // steps from the cycle start (off: its end, or the sketch's end)
  float hz = 0.0f;              // at its note-on (an Into glide's start)
  float ownHz = 0.0f;           // its own pitch (the step's; an Into glide's target): the core's trace (AP.11)
  float ratio = 1.0f, harmonic = 0.0f;  // the note-on's identity
  uint32_t source = 0;          // index into the held set
  ArpExact exact;               // what it plays relative to its source (Same: the source itself)
  int32_t position = -1, copy = 0;  // the status's pattern position / copy at its step (−1: none)
  uint32_t firstBend = 0, numBends = 0;  // its Pitch events in ArpSketch::bends
};

struct ArpSketch {
  std::vector<ArpSketchNote> notes;  // in note-on order
  std::vector<ArpSketchBend> bends;
  double cycle = 0.0;     // the cycle's length in steps (what the view spans)
  double stepMs = 0.0;    // one step (a Rate period) in ms
  bool truncated = false; // the cycle is longer than maxSteps
};

// The pitch (Hz) of note `n` at time `t` (steps) as the voice plays it: its note-on Hz, then each Pitch event from the
// value at its start toward its target over its length with its shape (a later event replaces a running one).
double arpSketchHzAt(const ArpSketch& s, const ArpSketchNote& n, double t) noexcept;

ArpSketch sketchArp(const ArpParams& params, const ArpLaneSteps& lanes, const ArpTables& tables,
                    std::span<const ArpSketchHeld> held, const ArpSketchConfig& config = {});

}  // namespace pq::engine
