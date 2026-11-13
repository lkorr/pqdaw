#pragma once

// DY.3: test access to the `transfer` display (transfer_element.cpp) and the meter (engine_elements.cpp): a golden's
// live values without a running engine, and what the transfer display drew.

#include <optional>

#include "pq/views/panel/element.h"

namespace pq::views::panel::test {

// Shows `db` as the transfer display's live input level (its dot). False: not a transfer display.
bool showTransferLevel(Element& e, double db);
// The output dB the transfer display draws at input dB `inDb` (as of its last paint; NaN: no curve), nullopt: not a
// transfer display.
std::optional<double> transferDrawn(const Element& e, double inDb);
// Shows a meter's two bars at `a`, `b` (mode level: linear amplitude, as RMS and peak; mode reduction: dB of reduction).
// False: not a meter.
bool showMeter(Element& e, float a, float b);

}  // namespace pq::views::panel::test
