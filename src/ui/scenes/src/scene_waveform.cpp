// The `waveform-lod` scene (T5.5): one stereo signal drawn by ui::drawWaveform at six zooms, from the coarse mip
// levels down to single samples (5000, 1500, 400, 60, 4 and 0.3 frames per device px). The overview levels are
// built here the way pq/media/waveform.h builds them (floor / ceil of min / max × 32767 per 256-frame bucket, ×4 mips)
// — ui does not depend on media; the media tests check that builder against brute force.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "pq/ui/waveform.h"
#include "scenes_internal.h"

namespace pq::ui::detail {

namespace {

struct LodSignal {
  static constexpr uint64_t kFrames = uint64_t{1} << 22;
  std::vector<float> ch[2];
  std::vector<std::vector<int16_t>> levels[2];  // per channel, per level: (min, max) pairs
  std::vector<uint64_t> fpb;
};

int16_t qMin(float x) { return static_cast<int16_t>(std::clamp(std::floor(x * 32767.0f), -32768.0f, 32767.0f)); }
int16_t qMax(float x) { return static_cast<int16_t>(std::clamp(std::ceil(x * 32767.0f), -32768.0f, 32767.0f)); }

const LodSignal& lodSignal() {
  static const LodSignal s = [] {
    LodSignal out;
    const uint64_t n = LodSignal::kFrames;
    out.ch[0].resize(n);
    out.ch[1].resize(n);
    uint32_t rng = 12345u;
    // Left: decaying hits every 24000 frames (noise + a 90 Hz thump); right: a sweep under a slow tremolo. Oscillators
    // by rotation (no per-sample trig), deterministic.
    double c = 1.0, sn = 0.0;
    const double w = 2.0 * 3.14159265358979 * 90.0 / 48000.0;
    const double cw = std::cos(w), sw = std::sin(w);
    double phase = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
      const uint64_t k = i % 24000;
      const double env = std::exp(-static_cast<double>(k) / 3000.0);
      rng = rng * 1664525u + 1013904223u;
      const double noise = (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) * 0.35;
      const double nc = c * cw - sn * sw, ns = c * sw + sn * cw;
      c = nc;
      sn = ns;
      out.ch[0][i] = static_cast<float>(env * (0.6 * sn + noise * (k < 6000 ? 1.0 : 0.3)));
      const double t = static_cast<double>(i) / static_cast<double>(n);
      phase += 2.0 * 3.14159265358979 * (40.0 + 2000.0 * t * t) / 48000.0;
      const double trem = 0.55 + 0.4 * std::sin(static_cast<double>(i) * 2.0 * 3.14159265358979 / 400000.0);
      out.ch[1][i] = static_cast<float>(trem * std::sin(phase));
    }
    for (int c2 = 0; c2 < 2; ++c2) {
      const std::vector<float>& x = out.ch[c2];
      std::vector<int16_t> l0((n + 255) / 256 * 2);
      for (uint64_t b = 0; b < (n + 255) / 256; ++b) {
        float lo = x[b * 256], hi = lo;
        for (uint64_t i = b * 256; i < std::min(n, b * 256 + 256); ++i) {
          lo = std::min(lo, x[i]);
          hi = std::max(hi, x[i]);
        }
        l0[2 * b] = qMin(lo);
        l0[2 * b + 1] = qMax(hi);
      }
      out.levels[c2].push_back(std::move(l0));
      while (out.levels[c2].back().size() > 2) {
        const std::vector<int16_t>& below = out.levels[c2].back();
        const size_t nb = below.size() / 2;
        std::vector<int16_t> up((nb + 3) / 4 * 2);
        for (size_t k = 0; k < (nb + 3) / 4; ++k) {
          int16_t lo = below[8 * k], hi = below[8 * k + 1];
          for (size_t j = 4 * k + 1; j < std::min(nb, 4 * k + 4); ++j) {
            lo = std::min(lo, below[2 * j]);
            hi = std::max(hi, below[2 * j + 1]);
          }
          up[2 * k] = lo;
          up[2 * k + 1] = hi;
        }
        out.levels[c2].push_back(std::move(up));
      }
    }
    for (size_t L = 0; L < out.levels[0].size(); ++L) out.fpb.push_back(uint64_t{256} << (2 * L));
    return out;
  }();
  return s;
}

}  // namespace

void waveformScene(DrawList& dl, const SceneTextures& tex, double /*time*/) {
  const LodSignal& sig = lodSignal();
  std::vector<WaveformLevel> levels(sig.fpb.size());
  for (size_t L = 0; L < levels.size(); ++L) {
    levels[L].framesPerBucket = sig.fpb[L];
    levels[L].buckets = sig.levels[0][L].size() / 2;
    levels[L].minMax[0] = sig.levels[0][L].data();
    levels[L].minMax[1] = sig.levels[1][L].data();
  }
  WaveformData data;
  data.channels = 2;
  data.frames = LodSignal::kFrames;
  data.levels = levels;
  data.samples.frame0 = 0;
  data.samples.frames = static_cast<int64_t>(LodSignal::kFrames);
  data.samples.channel[0] = sig.ch[0].data();
  data.samples.channel[1] = sig.ch[1].data();

  const float s = tex.scale.factor;
  const float W = static_cast<float>(dl.targetWidth()), H = static_cast<float>(dl.targetHeight());
  dl.addRectFilled({0, 0, W, H}, rgba(30, 30, 30));
  constexpr double kFpp[6] = {5000.0, 1500.0, 400.0, 60.0, 4.0, 0.3};
  const Rgba body[6] = {rgba(180, 160, 110), rgba(120, 170, 200), rgba(160, 190, 120), rgba(200, 140, 150), rgba(150, 140, 200),
                        rgba(200, 180, 120)};
  const float pad = std::round(8.0f * s);
  const float rowH = std::floor((H - pad) / 6.0f);
  for (int r = 0; r < 6; ++r) {
    const float y0 = std::round(pad + rowH * static_cast<float>(r));
    const float y1 = y0 + rowH - pad;
    const Rect rect{pad, y0, std::round(W - pad), y1};
    dl.addRectFilled(rect, body[r]);
    const double width = static_cast<double>(rect.x1 - rect.x0);
    // Centre each zoom on the same moment (a hit at 3 · 24000 + 400 frames, near the start), except the widest.
    const double centre = r == 0 ? width * kFpp[0] * 0.5 : 12.0 * 24000.0 + 400.0;
    WaveformDraw d;
    d.frame0 = centre - width * kFpp[r] * 0.5;
    d.frame1 = centre + width * kFpp[r] * 0.5;
    d.rect = rect;
    d.colour = rgba(28, 28, 28);
    d.gain = r == 3 ? 1.6f : 1.0f;  // one row with clip gain
    d.reverse = r == 4;             // and one reversed
    (void)drawWaveform(dl, data, d);
  }
}

}  // namespace pq::ui::detail
