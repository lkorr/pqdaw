#pragma once

// EQ.4: test access to the `response` display (response_elements.cpp): what it drew and where, without a feed or a
// GPU. Element-local world units.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
// EQ.4d: the display's three areas, top to bottom: 0 magnitude, 1 phase, 2 delay — open, and their element-local y span
// (as laid out by the last paint or pointer event); nullopt: not a response.
struct ResponseArea {
  bool open = false;
  float y0 = 0.0f, y1 = 0.0f;
};
std::optional<ResponseArea> area(const Element& e, int32_t a);
// The display's chips: the magnitude area's analyser / input / dB zoom, the areas' title chips (open / close; a closed
// area's whole bar), the phase area's WRAP / UNWRAP.
enum class ResponseChip : int32_t { Spectrum = 0, Input = 1, Zoom = 2, Magnitude = 3, Phase = 4, Wrap = 5, Delay = 6 };
ui::Vec2 chip(const Element& e, ResponseChip c);
// The phase area: unwrapped (else wrapped to ±180°), the total's value drawn at element x (degrees; NaN: none), the scale
// of the last paint, the x of each wrap marker the total drew (wrapped only).
bool phaseUnwrapped(const Element& e, const PanelTarget& t);
float phaseAt(const Element& e, float x);
std::pair<double, double> phaseScale(const Element& e);
std::vector<float> phaseWraps(const Element& e);
// The delay area: the total's group delay drawn at element x (ms; NaN: none), the scale of the last paint (lo: 0, or the
// most negative value drawn below the floor), the y of a value.
float delayAt(const Element& e, float x);
std::pair<double, double> delayScale(const Element& e);
float delayY(const Element& e, double ms);
// A `number` element's readout as drawn (its style's format: "Q 0.71", "×16"); nullopt: not a bound number.
std::optional<std::string> numberText(const Element& e, const PanelTarget& t);

}  // namespace pq::views::panel::test
