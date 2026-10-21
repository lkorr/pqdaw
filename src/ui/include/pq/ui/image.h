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
  uint64_t pixelsOverTolerance = 0;  // pixels with any channel differing by more than the tolerance
  uint64_t pixelCount = 0;
};
ImageDiff compareImages(const Image& a, const Image& b, uint32_t channelTolerance);

}  // namespace pq::ui
