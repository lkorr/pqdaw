#pragma once

// Private to the device view (T4.1): sizes shared by the chain view and the generic panel (logical px).

#include "pq/views/device_chain/device_chain.h"

namespace pq::views::device_chain {

inline constexpr float kTitleH = 20.0f;    // a device frame's title bar
inline constexpr float kFrameGap = 4.0f;   // between frames
inline constexpr float kChainPad = 4.0f;   // around the chain
inline constexpr float kAddW = 132.0f;     // the add slot at the end of the chain
inline constexpr float kPanelPad = 6.0f;   // inside a panel
inline constexpr float kRowH = 18.0f;      // one param row of the generic panel

}  // namespace pq::views::device_chain
