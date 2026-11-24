#pragma once

// AP.6: the arpeggiator panel's element kinds (arp_elements.cpp: presets, steplane, arppattern, arpbend; the kinds
// register with the generic ones, pq/views/panel/element.h). What tests read of them. UI thread only.

#include <cstdint>
#include <optional>

#include "pq/views/panel/element.h"

namespace pq::views::panel::test {

// A steplane's lit step (its lane's current step read from the note stage's status; −1: none, −2: not a steplane).
int32_t stepLaneLit(const Element& e);
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

}  // namespace pq::views::panel::test
