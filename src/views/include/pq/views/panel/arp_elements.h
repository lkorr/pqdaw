#pragma once

// AP.6: the arpeggiator panel's element kinds (arp_elements.cpp: presets, steplane, arppattern, arpbend, arppulse; the kinds
// register with the generic ones, pq/views/panel/element.h). What tests read of them. UI thread only.

#include <cstdint>
#include <optional>

#include "pq/views/panel/element.h"

namespace pq::views::panel::test {

// A steplane's lit step (its lane's current step read from the note stage's status; −1: none, −2: not a steplane).
int32_t stepLaneLit(const Element& e);
// AR.4: a steplane's cell of `step` (element-local; nullopt: not a steplane), the rows a grid lane shows now (± this
// many steps of its unit), the middle of its row of `value` (element-local y) and whether a value is being typed.
std::optional<ui::Rect> stepLaneCell(const Element& e, int32_t step);
int32_t stepLaneRange(const Element& e, const PanelTarget& t);
float stepLaneRowY(const Element& e, const PanelTarget& t, int32_t value);
bool stepLaneTyping(const Element& e);
// An arppattern's lit position (−1: none, −2: not one) and its pattern's entries.
int32_t patternPosition(const Element& e);
uint32_t patternCount(const Element& e);
// AP.10: an arppattern's resolved notes (0: not one), the generated (not source) ones among them, its lit note (−1:
// none) and how often it computed its pattern.
uint32_t resolvedCount(const Element& e);
uint32_t resolvedGenerated(const Element& e);
int32_t resolvedLit(const Element& e);
uint64_t resolvedComputes(const Element& e);
// AR.2: an arpbend's part `h` — 0 the Into knee (its time), 1 the note's head (Into), 2 the Out knee, 3 its tail (Out),
// 4 / 5 the middle of the Into / Out curve, 6 / 7 the previous / next step's ghost — element-local, read from target `t`
// now; nullopt: not shown (or not an arpbend).
std::optional<ui::Vec2> bendHandle(Element& e, const PanelTarget& t, int32_t h);
// AR.2: resolved note `note` of an arppattern (resolved from target `t` now): its head (or tail) as drawn,
// element-local; nullopt: none.
std::optional<ui::Vec2> patternNoteEnd(Element& e, const PanelTarget& t, uint32_t note, bool tail);

// AR.3: an arppulse's rows as it draws them from target `t` now (0: not one) — row `row`'s division, whether Auto
// locks it to its note's harmonic, whether a note is held on it, its mask and shift —, pulse `j`'s dot and the row's
// menu chip (element-local; nullopt: none), the row's lit pulse (−1: none, −2: not an arppulse), the playhead (the
// last pulse's place in the cycle, 0 … 1; −1: none), the count of coinciding-onset links drawn and how often it
// computed its rows.
struct PulseRow {
  int32_t division = 0;
  bool locked = false, held = false;
  uint32_t mask = 0;
  double shift = 0.0;
};
uint32_t pulseRows(Element& e, const PanelTarget& t);
PulseRow pulseRow(Element& e, const PanelTarget& t, uint32_t row);
std::optional<ui::Vec2> pulseDot(Element& e, const PanelTarget& t, uint32_t row, int32_t j);
std::optional<ui::Vec2> pulseMenu(Element& e, const PanelTarget& t, uint32_t row);
int32_t pulseLit(const Element& e, uint32_t row);
float pulsePlayhead(const Element& e);
uint32_t pulseLinks(const Element& e);
uint64_t pulseComputes(const Element& e);

}  // namespace pq::views::panel::test
