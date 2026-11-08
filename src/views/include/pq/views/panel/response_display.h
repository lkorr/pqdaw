#pragma once

// EQ.4: test access to the `response` display (response_elements.cpp): what it drew and where, without a feed or a
// GPU. Element-local world units.

#include <cstdint>
#include <optional>

#include "pq/views/devices/analysis.h"
#include "pq/views/panel/element.h"

namespace pq::views::panel::test {

// Shows `s` as the analyser's output spectrum (a golden's spectrum without a running engine). False: not a response.
bool showSpectrum(Element& e, devices::SpectrumSnapshot s);
// The total curve's dB drawn at element x (the nearest grid point; NaN: no curve), nullopt: not a response.
std::optional<float> drawnDb(const Element& e, float x);
// The frequency grid every display evaluates at, and grid point k's x on `e`.
uint32_t responsePoints() noexcept;
double responseHz(uint32_t k) noexcept;
float responseX(const Element& e, uint32_t k);
// Where band group `band`'s node is drawn (the active chain's), nullopt: none.
std::optional<ui::Vec2> nodeOf(const Element& e, doc::DeviceId band);
// The y of `db` (at the zoom of the last paint).
std::optional<float> yOfDb(const Element& e, double db);
// Whether its active chain's total is drawn as the product of the chain's bands (a pure series of them).
bool drawsSeries(const Element& e);
// The axis: x share t ∈ [0, 1] ↔ Hz (t = log(1 + f / 10 Hz) / log(2001)).
double axisHz(double t) noexcept;
double axisT(double hz) noexcept;

}  // namespace pq::views::panel::test
