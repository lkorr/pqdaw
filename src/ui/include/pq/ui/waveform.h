#pragma once

// Waveform drawing (T5.5; WF.1: one renderer, clean at every zoom): an audio file's min / max overview
// (pq/media/waveform.h; ui does not depend on media, so the overview arrives as the plain views below) and, below the
// finest level, its samples, drawn into a DrawList or written into a coverage image.
//
//   drawWaveform(dl, data, {frame0, frame1, rect, gain, reverse, colour, lineWidth, envelope})
//
// draws source frames [frame0, frame1) across `rect` (left to right; mirrored when `reverse`). Columns are whole
// device px from rect.x0 (an integer, as clip edges are); only the columns inside the draw list's current clip rect (and
// [clipX0, clipX1)) are generated, so the cost is proportional to the visible pixels, never to the file length.
// Channels are stacked lanes (channel 0 on top; `merge`: all in one lane, one over the other), each centred on its zero
// line, which sits on a pixel centre (silence is one full row and its feather at lineWidth 1); `gain` scales the amplitude, clamped to
// the lane.
//
// What a column shows, with f frames per device px:
//   f ≥ 512 (twice the finest bucket)     the coarsest overview level whose bucket is ≤ f / 2 (2 … 9 buckets per
//                                         column); columns whose ranges do not meet are joined at the midpoint of the
//                                         gap (the signal is continuous: a slow wave is one stroke, not dashes)
//   2 ≤ f < 512                           the samples (`data.samples`, when it covers the columns and
//                                         kWaveformSampleMargin frames either side, clamped to the file): min / max
//                                         of the column's frames and of the line between samples at its two edges
//   f < 2                                 the band-limited curve through the samples (a 16-tap Hann-windowed sinc; it
//                                         passes through every sample): its values at the column's edges (and its
//                                         middle above 0.5 frames per px) and the samples inside it
//   f ≤ 1/6                               also a dot on every sample (fading in from 6 to 10 px per sample)
// Without samples below 512 frames per px: the finest level stretched (WaveformStats::approx; only while the samples
// load).
//
// Antialiasing (the same in every regime, so nothing pops between them): a column is a vertical coverage profile, a
// trapezoid. A column whose range overlaps its neighbours' (a dense body) is solid from its min to its max (at least
// lineWidth tall) with its top and bottom edges at fractional rows (box-filter coverage). A column whose range only
// touches its neighbours' (one stroke passing through: a slow wave, the curve) is the exact coverage of a line of
// weight lineWidth crossing the column: ramps as long as the stroke's rise, so a steep stroke is antialiased sideways
// too. In between, by how much the ranges overlap.
// WF.2: the edge of a THIN column (a stroke, a turn, silence: no more than a line's weight in it besides the stroke) is
// the shared line's (pq/ui/lines.h): opaque to the core's edge, kSmoothFeather (0.75 px down the column) fading
// out beyond it — so a waveform's stroke weighs what every other curve does and does not "rope" along a shallow slope
// (a 1 px box-filtered line is one full row where it lies on a row and two half rows where it lies between two). A body
// keeps the pixel box: exact coverage in its edge rows. Between the two over 2 px of body.
// WF.2: a tone of about two columns per cycle puts half a cycle in each column: the ranges alternate and beat against
// the pixel grid (a moiré). A column in the middle of three strict alternations of its neighbours' midpoints takes
// the union of its neighbours' ranges (the tone's envelope), by how large the alternation is against that union.
//
// Geometry: per lane and run of columns with data, one strip with four vertices at each column centre (alpha 0, a, a,
// 0 down the profile) plus four at each end of the run: the GPU samples each pixel at its centre, so a pixel gets its
// column's profile there. rasterWaveform writes the same profile into a coverage image (within 2 / 255 of
// rasterizeCoverage of the geometry: the tests bound it), row by row (AVX2: eight columns a step). UI thread;
// allocation-free once its scratch has grown.

#include <cstdint>
#include <span>

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"

namespace pq::ui {

inline constexpr uint32_t kMaxWaveformChannels = 8;  // more channels draw the first eight
// The frames either side of the visible ones a sample window must hold for the curve (the sinc's reach + 1); a caller
// asks pq/views/waveforms.h for [first − margin, last + margin) (it clamps to the file).
inline constexpr int64_t kWaveformSampleMargin = 9;
// The curve gets a dot on each sample from kWaveformDotsFromPx device px per sample, opaque at kWaveformDotsFullPx.
inline constexpr float kWaveformDotsFromPx = 6.0f;
inline constexpr float kWaveformDotsFullPx = 10.0f;

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
  // Columns are generated only inside [clipX0, clipX1) (and the draw list's clip rect): a caller that clips without
  // pushing a clip rect (which would split draw commands per clip).
  float clipX0 = -3.0e38f;
  float clipX1 = 3.0e38f;
  // WF.1: the weight of a stroke and the least height of a column, device px (a view passes its UI scale).
  float lineWidth = 1.0f;
  // WF.1: every channel in one lane, one drawn over the other (a small display: the Sampler's closed well).
  bool merge = false;
  // WF.1: no dots on the samples (a caller that draws the same frames twice, e.g. a dim copy under a lit one).
  bool dots = true;
  // SX.3: a gain per device column, applied at draw time over the peaks / samples (an audio clip's fades and
  // crossfades: pq/core/clip_envelope.h), on top of `gain`: the column at device x (whole px) takes
  // envelope[x − envelopeX0], clamped to the array's ends. Null: none. The caller keeps it alive for the call.
  const float* envelope = nullptr;
  int32_t envelopeX0 = 0;
  uint32_t envelopeCount = 0;
};

struct WaveformStats {
  uint32_t columns = 0;   // columns with data
  uint32_t vertices = 0;
  int32_t level = -2;     // the level drawn from; −1 = the samples; −2 = nothing
  bool approx = false;    // below 512 frames per px without samples: the finest level stretched
  bool curve = false;     // WF.1: below 2 frames per px from the samples: the band-limited curve
  uint32_t dots = 0;      // WF.1: sample dots drawn (channel 0's)
};

WaveformStats drawWaveform(DrawList& dl, const WaveformData& data, const WaveformDraw& d);
// The same columns written straight into an 8-bit coverage image (each pixel the larger of what it holds and its
// column's profile at its centre; pixels are never cleared): a tile of the waveform without tessellating and
// rasterising triangles. `d.rect` is in the image's pixels; columns outside [0, width) are skipped.
WaveformStats rasterWaveform(const WaveformData& data, const WaveformDraw& d, uint8_t* dst, uint32_t width, uint32_t height,
                             uint32_t rowPitch);

// The level drawWaveform reads at `framesPerPx` (−1 = samples wanted: below the finest level).
int32_t waveformLevelFor(const WaveformData& data, double framesPerPx) noexcept;

// The band-limited value drawWaveform's curve has at `position` (frames; sample j sits at j) of one channel's
// `frames` samples starting at file frame `frame0` (frames outside read 0). Tests; a view that reads a value off the
// curve.
float waveformCurveAt(const float* samples, int64_t frame0, int64_t frames, double position) noexcept;

// WF.1: columns a view computed itself (the audio clip view's peak body and RMS core, which are envelopes, not the
// signal's min / max) drawn with the renderer's geometry: column i (a whole device px at x0 + i) covers lo[i] … hi[i]
// (−1 … 1 about `centre`, `half` px per unit; lo > hi: no column), antialiased as above, clamped to [y0, y1].
struct WaveformColumns {
  const float* lo = nullptr;
  const float* hi = nullptr;
  uint32_t count = 0;
  float x0 = 0.0f;
  float y0 = 0.0f, y1 = 0.0f;
  float centre = 0.0f, half = 0.0f;
  float lineWidth = 1.0f;
  Rgba colour = 0xffffffffu;
  float clipX0 = -3.0e38f, clipX1 = 3.0e38f;
};
uint32_t drawWaveformColumns(DrawList& dl, const WaveformColumns& c);  // returns the columns drawn

}  // namespace pq::ui
