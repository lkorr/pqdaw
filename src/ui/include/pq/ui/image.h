#pragma once

// CPU-side RGBA8 images: PNG encode/decode (screenshots, golden files) and tolerance comparison.

#include <cstdint>
#include <string>
#include <vector>

#include "pq/core/result.h"

namespace pq::ui {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // width*height*4, rows top to bottom, straight alpha

  uint8_t* pixel(uint32_t x, uint32_t y) noexcept { return rgba.data() + (size_t(y) * width + x) * 4; }
  const uint8_t* pixel(uint32_t x, uint32_t y) const noexcept { return rgba.data() + (size_t(y) * width + x) * 4; }
};

// PNG (8-bit RGBA, non-interlaced). Encoder picks a filter per row (min sum of abs heuristic) and
// deflates with zlib level 6. Returns an empty vector only if zlib runs out of memory.
std::vector<uint8_t> encodePng(const Image& img);
// Decodes 8-bit greyscale / grey+alpha / RGB / RGBA / palette PNGs (non-interlaced) into RGBA8, which covers
// everything our encoder and common tools write for golden files. Other variants return Unsupported.
Result<Image> decodePng(const uint8_t* data, size_t size);

Status writePng(const std::string& path, const Image& img);
Result<Image> readPng(const std::string& path);

struct ImageDiff {
  bool sameSize = false;
  uint32_t maxChannelDiff = 0;     // over all pixels and channels
  uint64_t pixelsOverTolerance = 0;  // pixels with any channel differing by more than the tolerance ("outliers")
  uint64_t pixelCount = 0;
  uint32_t channelTolerance = 0;     // the tolerance the diff was computed with
  // Outliers with at least one other outlier among their 8 neighbours. GPU/driver noise is isolated pixels;
  // a missing glyph, dot or shifted trace is a connected patch.
  uint64_t clusteredOutliers = 0;
  // Bounding box of the outliers, [x0, x1) × [y0, y1); empty (x0 == x1) when there are none.
  uint32_t boxX0 = 0, boxY0 = 0, boxX1 = 0, boxY1 = 0;
};
ImageDiff compareImages(const Image& a, const Image& b, uint32_t channelTolerance);

// Golden-image acceptance (T0.7-fix5): every channel within `channel` of the golden, except at most
// `outliersPerMillion` pixels per million, each of which must be isolated (no outlier among its 8
// neighbours) and within `outlierCap`. The allowance is for rasterisation and filtering differences between
// GPU vendors and drivers; it is far too small and too scattered to hide a dropped glyph or a 1 px shift.
struct GoldenTolerance {
  uint32_t channel = 2;
  uint32_t outlierCap = 24;
  uint64_t outliersPerMillion = 100;  // 38 px at 800×480
};
// Whether `d` passes `t`. `d` must have been computed with channelTolerance == t.channel (false otherwise).
bool passesGolden(const ImageDiff& d, const GoldenTolerance& t = {}) noexcept;
// Diff visualisation for failure output: the expected image dimmed to a quarter, pixels that differ within
// the tolerance in blue, outliers in yellow (≤ cap) or red (> cap) with brightness rising with the
// difference, and the outliers' bounding box outlined in magenta one pixel outside it. Sizes must match
// (returns an empty image otherwise).
Image diffImage(const Image& actual, const Image& expected, uint32_t channelTolerance, uint32_t outlierCap = 24);

}  // namespace pq::ui
