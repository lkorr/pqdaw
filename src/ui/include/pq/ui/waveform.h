#pragma once

// Waveform drawing (T5.5): an audio file's min / max overview (pq/media/waveform.h; ui does not depend on media, so
// the overview arrives as the plain views below) drawn into a DrawList, one triangle strip per channel.
//
//   drawWaveform(dl, data, {frame0, frame1, rect, gain, reverse, colour})
//
// draws source frames [frame0, frame1) across `rect` (left to right; mirrored when `reverse`). Columns are whole
// device px from rect.x0 (an integer, as clip edges are); only the columns inside the draw list's current clip rect are
// generated, so the cost is proportional to the visible pixels, never to the file length. Level of detail: with f
// frames per device px, the coarsest level whose bucket is ≤ f (each column reduces ≤ 5 buckets); below the finest
// level's 256 frames per px, the samples themselves when `data.samples` covers the columns (≥ 1 frame per px: min /
// max of the frames; < 1: the line between samples), else the finest level stretched (WaveformStats::approx).
// Channels are stacked lanes (channel 0 on top), each centred on its zero line; `gain` scales the amplitude (vertex y),
// clamped to the lane. Every column is at least 1 px tall, so silence draws as a line.
//
// Geometry: per channel and run of columns with data, one strip with two vertices (top, bottom) at each column centre
// plus one pair at each end of the run, solid colour, no feather: the GPU samples each pixel at its centre, so a
// column covers exactly the rows of its [min, max]. rasterizeCoverage (pq/ui/raster.h) reproduces it exactly, so a
// tile of the same geometry equals the direct draw. UI thread; allocation-free once its scratch has grown.

#include <cstdint>
#include <span>

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"

namespace pq::ui {

inline constexpr uint32_t kMaxWaveformChannels = 8;  // more channels draw the first eight

struct WaveformLevel {
  uint64_t framesPerBucket = 0;
  uint64_t buckets = 0;  // readable buckets (a progressive overview: the ones built so far)
  const int16_t* minMax[kMaxWaveformChannels] = {};  // per channel: pairs (min, max), int16 full scale = 32767
};

struct WaveformSamples {
  int64_t frame0 = 0;
  int64_t frames = 0;  // 0 = none
  const float* channel[kMaxWaveformChannels] = {};
};

struct WaveformData {
  uint32_t channels = 0;
  uint64_t frames = 0;                    // the file's length in frames
  std::span<const WaveformLevel> levels;  // finest first, each kOverviewMipFactor times coarser
  WaveformSamples samples;                // optional deep-zoom data
};

struct WaveformDraw {
  double frame0 = 0.0;  // source frame at rect.x0 (reverse: at rect.x1)
  double frame1 = 0.0;  // source frame at rect.x1 (reverse: at rect.x0); > frame0
  Rect rect{};
  float gain = 1.0f;
  bool reverse = false;
  Rgba colour = 0xffffffffu;
};

struct WaveformStats {
  uint32_t columns = 0;   // columns with data
  uint32_t vertices = 0;
  int32_t level = -2;     // the level drawn from; −1 = the samples; −2 = nothing
  bool approx = false;    // below 256 frames per px without samples: the finest level stretched
};

WaveformStats drawWaveform(DrawList& dl, const WaveformData& data, const WaveformDraw& d);

// The level drawWaveform reads at `framesPerPx` (−1 = samples wanted: below the finest level).
int32_t waveformLevelFor(const WaveformData& data, double framesPerPx) noexcept;

}  // namespace pq::ui
