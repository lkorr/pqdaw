#pragma once

// The colour of a sounding pitch by its frequency (TMM.17; TU.2 shares it): a log-frequency spectrum, red below
// 30 Hz, rose ~85 Hz, violet ~180 Hz, blue ~440 Hz, cyan ~620 Hz, green ~820 Hz, yellow from 1050 Hz up. The Phase
// Accumulator's orbit (panel/rich_elements.cpp) colours its voices with it and the MOS circle (mos_wheel/mos_circle.cpp)
// its lit degrees, so a note reads the same colour in both. UI thread; a log2 and a short scan per call.

#include <cmath>
#include <iterator>

#include "pq/ui/color.h"

namespace pq::views {

inline ui::Rgba voiceColour(double hz) noexcept {
  struct HueStop {
    float log2Hz;
    ui::Rgba colour;
  };
  static constexpr HueStop kHues[] = {
      {4.9069f, ui::hex(0xE83434)}, {6.4094f, ui::hex(0xF0405A)}, {7.4919f, ui::hex(0xB054EC)}, {8.7814f, ui::hex(0x5078FF)},
      {9.2761f, ui::hex(0x28C4EC)}, {9.6795f, ui::hex(0x58DE78)}, {10.0362f, ui::hex(0xFCDC40)},
  };
  constexpr size_t n = std::size(kHues);
  if (!(hz > 0.0)) return kHues[0].colour;
  const float l = static_cast<float>(std::log2(hz));
  if (l <= kHues[0].log2Hz) return kHues[0].colour;
  for (size_t i = 1; i < n; ++i)
    if (l < kHues[i].log2Hz) return ui::lerp(kHues[i - 1].colour, kHues[i].colour, (l - kHues[i - 1].log2Hz) / (kHues[i].log2Hz - kHues[i - 1].log2Hz));
  return kHues[n - 1].colour;
}

}  // namespace pq::views
