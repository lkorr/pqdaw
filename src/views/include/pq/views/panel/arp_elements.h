#pragma once

// AP.6: the arpeggiator panel's element kinds (arp_elements.cpp: presets, steplane, arppattern, rampshape; the kinds
// register with the generic ones, pq/views/panel/element.h). What tests read of them. UI thread only.

#include <cstdint>

#include "pq/views/panel/element.h"

namespace pq::views::panel::test {

// A steplane's lit step (its lane's current step read from the note stage's status; −1: none, −2: not a steplane).
int32_t stepLaneLit(const Element& e);
// An arppattern's lit position (−1: none, −2: not one) and its pattern's entries.
int32_t patternPosition(const Element& e);
uint32_t patternCount(const Element& e);

}  // namespace pq::views::panel::test
