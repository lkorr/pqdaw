#pragma once

// Theme tokens. SPEC §11: the DAW chrome will use Ableton's dark-grey token set (added with the widget
// toolkit); PQ Meter's palette below applies to signal displays only. Values are copied from PQ Meter's
// src/ui/theme.h so ported visualisers look identical.

#include "pq/ui/color.h"

namespace pq::ui::signal_theme {

inline constexpr Rgba kBackground = rgba(10, 10, 14);   // #0A0A0E
inline constexpr Rgba kPanel = rgba(16, 16, 22);        // #101016
inline constexpr Rgba kHeader = rgba(24, 24, 32);       // #181820
inline constexpr Rgba kHeaderText = rgba(140, 140, 160);
inline constexpr Rgba kGrid = rgba(255, 255, 255, 14);
inline constexpr Rgba kGridLabel = rgba(255, 255, 255, 70);
inline constexpr Rgba kAccent = rgba(120, 190, 255);    // #78BEFF
inline constexpr Rgba kViolet = rgba(172, 142, 255);    // #AC8EFF
inline constexpr Rgba kWaveform = rgba(120, 220, 160);
inline constexpr Rgba kWaveformR = rgba(255, 150, 120);

// Multiband band colours, low → high (PQ Meter Theme::bandColor, converted from float to 8-bit).
inline constexpr Rgba kBands[6] = {
    rgba(242, 92, 64), rgba(255, 201, 61), rgba(110, 217, 255),
    rgba(158, 242, 140), rgba(199, 153, 255), rgba(255, 140, 204),
};

// PQ Meter's "Magma" colormap stops (t, r, g, b), used by the test pattern's textured ramp.
struct RampStop {
  float t, r, g, b;
};
inline constexpr RampStop kMagma[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.15f, 0.090f, 0.040f, 0.220f}, {0.35f, 0.360f, 0.070f, 0.430f},
    {0.55f, 0.720f, 0.150f, 0.320f}, {0.75f, 0.960f, 0.470f, 0.130f}, {0.90f, 0.990f, 0.810f, 0.230f},
    {1.00f, 1.000f, 1.000f, 0.930f},
};

}  // namespace pq::ui::signal_theme
