// The MOS explorer's data colours, shared by the region chart and the circle (TU.2b: one palette, so a circle's L
// colour is its region's band colour in the chart): tuning::kMosPalette (region collapsed / equalized colours) and
// tuning::temperamentColor, parsed from the explorer's "#rrggbb[aa]" strings.
//
// UI thread only. Nothing here is RT-safe.

#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "pq/tuning/mos_data.h"
#include "pq/tuning/mos_regions.h"
#include "pq/ui/color.h"

namespace pq::views::mos_palette {

constexpr uint32_t hexDigit(char c) noexcept {
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<uint32_t>(c - 'A' + 10);
  return 0;
}

// "#rrggbb" or "#rrggbbaa"; anything else is the explorer's #888888.
constexpr ui::Rgba parseHex(std::string_view s) noexcept {
  auto byte = [&](size_t i) { return hexDigit(s[i]) * 16 + hexDigit(s[i + 1]); };
  if (s.size() < 7 || s[0] != '#') return ui::rgba(136, 136, 136);
  return ui::rgba(byte(1), byte(3), byte(5), s.size() >= 9 ? byte(7) : 255u);
}

// kMosPalette[index mod 30] (a tuning::MosRegion collapsedColor / equalizedColor).
inline ui::Rgba region(uint32_t index) noexcept {
  static constexpr auto kParsed = [] {
    std::array<ui::Rgba, tuning::kMosPalette.size()> a{};
    for (size_t i = 0; i < a.size(); ++i) a[i] = parseHex(tuning::kMosPalette[i]);
    return a;
  }();
  return kParsed[index % kParsed.size()];
}

// The explorer's colour of a named temperament, if it has one.
inline std::optional<ui::Rgba> temperament(std::string_view name) noexcept {
  const auto s = tuning::temperamentColor(name);
  if (!s) return std::nullopt;
  return parseHex(*s);
}

}  // namespace pq::views::mos_palette
