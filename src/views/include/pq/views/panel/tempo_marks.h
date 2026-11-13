#pragma once

// DT.3 (docs/plans/DT.md "DT.3"): the TEMPO SLIDER's markings (pq/views/src/panel/tempo_slider.cpp) — note values named
// as reduced fractions of a whole note, shown by simplicity and by the room on screen.
//
//   candidates  n/d of a whole note, d ∈ {1, 2, 4 … 64} · {1, 3, 5, 7}, n ≤ 8, reduced, plus every sync value
//               (doc::noteValues(), named the same way: "3/128"), whose duration at the tempo lies inside the time
//               curve's range. Ordered by SIMPLICITY: tier 0 the powers of two (1/4, 1/64, 2/1); tier 1 the threes —
//               dotted (3/16) before triplets (1/12); tier 2 the fives (1/5, 1/20); tier 3 the sevens (1/7, 2/7); tier 4
//               other numerators (5/16, 3/7); within a tier by numerator, then denominator.
//   visibility  in that order, a tick is shown when it is at least kTickGapPx[tier] LOGICAL screen px (panel units × the
//               host's zoom) from every tick already shown, and labelled when its text clears every label already
//               placed and the element's edges — so a wider slider (a stretched face) or a closer zoom shows more of
//               both. Label text is kTextPx panel units, shrinking to half that as the zoom passes 2× (it stays ~7
//               screen px: a closer zoom makes room for more labels too).
//
// UI thread only; layoutTempoMarks allocates nothing once `marks` exists.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/dsp/curve.h"

namespace pq::views::panel {

struct TempoMark {
  uint16_t num = 1, den = 4;  // the reduced fraction of a whole note
  uint8_t tier = 0;           // 0 powers of two, 1 threes, 2 fives, 3 sevens, 4 other numerators
  double seconds = 0.0;       // at the tempo
  double pos = 0.0;           // the time knob's position for it
  std::string label;          // "3/16"
  bool shown = false, labelled = false;
  float x = 0.0f, half = 0.0f;  // layout: element-local x, the label's half width
};

// Logical screen px a tick of each tier needs from every tick already shown: the plain values pack closest, the rarer
// families need more room (so the default slider shows the straight, dotted and triplet values and a wider one more).
inline constexpr float kTickGapPx[5] = {3.0f, 3.0f, 6.0f, 8.0f, 10.0f};
inline constexpr float kMarkTextPx = 7.0f;  // label size, panel units at zoom ≤ 1

// The candidates at `bpm` inside `time`'s range (a Hz curve: placed at 1 / seconds), in simplicity order.
std::vector<TempoMark> tempoMarkCandidates(const dsp::Curve& time, double bpm);
// Which are shown and labelled for a groove from x0 to x1 (element-local panel units) in an element `width` wide, at
// `pxPerUnit` logical screen px per panel unit. Returns the label text size used (panel units).
float layoutTempoMarks(std::span<TempoMark> marks, float x0, float x1, float width, float pxPerUnit);

}  // namespace pq::views::panel
