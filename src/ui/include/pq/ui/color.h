#pragma once

// Packed 8-bit colours in ImGui's IM_COL32 layout: R in the low byte, so the bytes in memory are R, G, B, A.
// That is what the vertex format's `col` field and VK_FORMAT_R8G8B8A8_UNORM expect, and it lets PQ Meter's
// IM_COL32-based code port without byte swizzling. Colours are straight (not premultiplied) alpha, in
// gamma-encoded sRGB values blended as-is (no linearisation), exactly like PQ Meter's GL output.

#include <algorithm>
#include <cstdint>

namespace pq::ui {

using Rgba = uint32_t;

constexpr Rgba rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) noexcept {
  return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

// 0xRRGGBB literal (as written in design docs) with an explicit alpha.
constexpr Rgba hex(uint32_t rrggbb, uint32_t a = 255) noexcept {
  return rgba((rrggbb >> 16) & 0xFFu, (rrggbb >> 8) & 0xFFu, rrggbb & 0xFFu, a);
}

constexpr uint32_t redOf(Rgba c) noexcept { return c & 0xFFu; }
constexpr uint32_t greenOf(Rgba c) noexcept { return (c >> 8) & 0xFFu; }
constexpr uint32_t blueOf(Rgba c) noexcept { return (c >> 16) & 0xFFu; }
constexpr uint32_t alphaOf(Rgba c) noexcept { return c >> 24; }

constexpr Rgba withAlpha(Rgba c, uint32_t a) noexcept { return (c & 0x00FFFFFFu) | ((a & 0xFFu) << 24); }

// Alpha scaled by k (clamped to [0, 1]); RGB unchanged.
inline Rgba mulAlpha(Rgba c, float k) noexcept {
  const float a = static_cast<float>(alphaOf(c)) * std::clamp(k, 0.0f, 1.0f);
  return withAlpha(c, static_cast<uint32_t>(a + 0.5f));
}

// The signal-display colour helpers below are PQ Meter's (src/modules/spectrum.cpp), with its exact rounding.

// Alpha replaced by a in [0, 1] (rounded), rather than scaled (spectrum.cpp setAlpha).
inline Rgba setAlpha(Rgba c, float a) noexcept {
  return withAlpha(c, static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

// RGB multiplied by k (truncated, clamped to 0..255); alpha unchanged (spectrum.cpp scaleRgb).
inline Rgba scaleRgb(Rgba c, float k) noexcept {
  auto ch = [k](uint32_t v) { return static_cast<uint32_t>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
  return rgba(ch(redOf(c)), ch(greenOf(c)), ch(blueOf(c)), alphaOf(c));
}

// RGB moved toward white by amt (truncated); alpha unchanged (spectrum.cpp brighten: the curve's bright core).
inline Rgba brighten(Rgba c, float amt) noexcept {
  auto up = [amt](uint32_t v) {
    const float f = static_cast<float>(v);
    return static_cast<uint32_t>(std::min(255.0f, f + (255.0f - f) * amt));
  };
  return rgba(up(redOf(c)), up(greenOf(c)), up(blueOf(c)), alphaOf(c));
}

// Per-channel linear interpolation, t in [0, 1]. Used for gradient vertex colours.
inline Rgba lerp(Rgba a, Rgba b, float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [t](uint32_t x, uint32_t y) {
    return static_cast<uint32_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f);
  };
  return rgba(ch(redOf(a), redOf(b)), ch(greenOf(a), greenOf(b)), ch(blueOf(a), blueOf(b)), ch(alphaOf(a), alphaOf(b)));
}

}  // namespace pq::ui
