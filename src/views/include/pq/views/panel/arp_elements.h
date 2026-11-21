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
// AP.10: an arpbend's handle `h` (0 Into time, 1 Into offset, 2 Out time, 3 Out offset), element-local, read from
// target `t` now; nullopt: not shown (or not an arpbend).
std::optional<ui::Vec2> bendHandle(Element& e, const PanelTarget& t, int32_t h);

}  // namespace pq::views::panel::test
