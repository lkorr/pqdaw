#pragma once

// A media::WaveformOverview (and optionally a sample window) as the plain ui::WaveformData ui::drawWaveform reads
// (T5.5 / T5.6). Shared by the arrangement and the audio clip view.

#include <algorithm>
#include <cstddef>

#include "pq/media/waveform.h"
#include "pq/ui/waveform.h"

namespace pq::views {

inline constexpr std::size_t kMaxOverviewLevels = 32;  // 256 · 4^31 frames: any file

// The overview's readable levels (what is built so far) and the window's samples, in `lv` (the caller's storage).
inline ui::WaveformData waveformData(const media::WaveformOverview& o, const media::SampleWindow* w,
                                     ui::WaveformLevel (&lv)[kMaxOverviewLevels]) noexcept {
  ui::WaveformData d;
  d.channels = std::min(o.channels(), ui::kMaxWaveformChannels);
  d.frames = o.frames();
  const std::size_t n = std::min(o.levelCount(), kMaxOverviewLevels);
  for (std::size_t i = 0; i < n; ++i) {
    lv[i] = {};
    lv[i].framesPerBucket = o.level(i).framesPerBucket;
    lv[i].buckets = o.validBuckets(i);
    for (uint32_t c = 0; c < d.channels; ++c) lv[i].minMax[c] = o.minMax(i, c);
  }
  d.levels = {lv, n};
  if (w && w->frames > 0 && w->channels >= d.channels) {
    d.samples.frame0 = w->frame0;
    d.samples.frames = w->frames;
    for (uint32_t c = 0; c < d.channels; ++c) d.samples.channel[c] = w->channel(c);
  }
  return d;
}

}  // namespace pq::views
