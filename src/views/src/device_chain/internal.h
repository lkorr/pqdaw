#pragma once

// Private to the device view (T4.1): its sizes (logical px).

#include "pq/views/device_chain/device_chain.h"

namespace pq::views::device_chain {

inline constexpr float kTitleH = 20.0f;    // a device panel's title bar (T4.6's frame)
inline constexpr float kFrameGap = 4.0f;   // between frames
inline constexpr float kChainPad = 4.0f;   // around the chain
inline constexpr float kAddW = 132.0f;     // the add slot at the end of the chain

}  // namespace pq::views::device_chain
