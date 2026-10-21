#include "pq/ui/colormap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace pq::ui {

namespace {

// PQ Meter src/ui/theme.h, Theme::rampStops. Each ramp starts at black so silence reads as background
// (except Pastel, whose floor is a deep lilac on purpose).
constexpr RampStop kInferno[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.14f, 0.230f, 0.020f, 0.010f}, {0.32f, 0.560f, 0.060f, 0.020f},
    {0.50f, 0.820f, 0.180f, 0.020f}, {0.68f, 0.960f, 0.420f, 0.020f}, {0.84f, 1.000f, 0.700f, 0.090f},
    {0.94f, 1.000f, 0.910f, 0.380f}, {1.00f, 1.000f, 1.000f, 0.900f},
};
constexpr RampStop kViridis[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.20f, 0.150f, 0.100f, 0.320f}, {0.40f, 0.180f, 0.330f, 0.550f},
    {0.60f, 0.130f, 0.560f, 0.550f}, {0.80f, 0.420f, 0.760f, 0.350f}, {1.00f, 0.990f, 0.910f, 0.150f},
};
constexpr RampStop kGray[] = {
    {0.00f, 0.000f, 0.000f, 0.000f},
    {1.00f, 1.000f, 1.000f, 1.000f},
};
constexpr RampStop kIce[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.25f, 0.030f, 0.110f, 0.290f}, {0.50f, 0.060f, 0.360f, 0.640f},
    {0.75f, 0.310f, 0.700f, 0.900f}, {1.00f, 0.900f, 0.990f, 1.000f},
};
constexpr RampStop kGold[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.16f, 0.130f, 0.080f, 0.020f}, {0.34f, 0.360f, 0.220f, 0.050f},
    {0.52f, 0.620f, 0.420f, 0.100f}, {0.68f, 0.850f, 0.640f, 0.180f}, {0.82f, 0.980f, 0.820f, 0.340f},
    {0.93f, 1.000f, 0.930f, 0.600f}, {1.00f, 1.000f, 0.990f, 0.880f},
};
constexpr RampStop kSilver[] = {
    {0.00f, 0.010f, 0.008f, 0.020f}, {0.18f, 0.115f, 0.100f, 0.165f}, {0.36f, 0.275f, 0.250f, 0.355f},
    {0.54f, 0.455f, 0.430f, 0.545f}, {0.70f, 0.650f, 0.630f, 0.730f}, {0.84f, 0.830f, 0.820f, 0.885f},
    {0.94f, 0.930f, 0.925f, 0.960f}, {1.00f, 0.995f, 0.995f, 1.000f},
};
constexpr RampStop kPastel[] = {
    {0.00f, 0.090f, 0.070f, 0.130f}, {0.16f, 0.420f, 0.360f, 0.580f}, {0.34f, 0.560f, 0.700f, 0.880f},
    {0.50f, 0.620f, 0.880f, 0.800f}, {0.66f, 0.800f, 0.930f, 0.700f}, {0.80f, 0.990f, 0.930f, 0.680f},
    {0.91f, 0.990f, 0.790f, 0.720f}, {1.00f, 0.990f, 0.910f, 0.910f},
};
constexpr RampStop kNeon[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.18f, 0.030f, 0.160f, 0.240f}, {0.36f, 0.020f, 0.520f, 0.560f},
    {0.52f, 0.100f, 0.800f, 0.520f}, {0.66f, 0.520f, 0.880f, 0.260f}, {0.78f, 0.930f, 0.640f, 0.320f},
    {0.90f, 1.000f, 0.260f, 0.640f}, {1.00f, 1.000f, 0.780f, 0.960f},
};
constexpr RampStop kCopper[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.18f, 0.020f, 0.120f, 0.110f}, {0.36f, 0.060f, 0.320f, 0.280f},
    {0.52f, 0.320f, 0.420f, 0.280f}, {0.68f, 0.680f, 0.400f, 0.200f}, {0.84f, 0.900f, 0.520f, 0.260f},
    {1.00f, 1.000f, 0.780f, 0.560f},
};

constexpr std::span<const RampStop> kRamps[kColormapCount] = {
    signal_theme::kMagma, kInferno, kViridis, kGray, kIce, kGold, kSilver, kPastel, kNeon, kCopper,
};
constexpr std::string_view kNames[kColormapCount] = {"Magma", "Inferno", "Viridis", "Grayscale", "Ice",
                                                     "Gold",  "Silver",  "Pastel",  "Neon",      "Copper"};

int clampIndex(Colormap m) noexcept { return std::clamp(static_cast<int>(m), 0, kColormapCount - 1); }

uint32_t to8(float x) noexcept { return static_cast<uint32_t>(static_cast<int>(x * 255.0f + 0.5f)); }

// PaletteTrack::lerpU32: per channel, rounded, alpha forced opaque.
Rgba lerpOpaque(Rgba a, Rgba b, float u) noexcept {
  auto ch = [u](uint32_t x, uint32_t y) {
    const float fx = static_cast<float>(x);
    return static_cast<uint32_t>(static_cast<int>(fx + (static_cast<float>(y) - fx) * u + 0.5f));
  };
  return rgba(ch(redOf(a), redOf(b)), ch(greenOf(a), greenOf(b)), ch(blueOf(a), blueOf(b)), 255);
}

}  // namespace

std::string_view colormapName(Colormap m) noexcept { return kNames[clampIndex(m)]; }
std::span<const RampStop> colormapStops(Colormap m) noexcept { return kRamps[clampIndex(m)]; }

const ColorLut& colormapLut(Colormap m) noexcept {
  static const std::array<ColorLut, kColormapCount> luts = [] {
    std::array<ColorLut, kColormapCount> out{};
    for (int i = 0; i < kColormapCount; ++i) buildLut(kRamps[i], out[static_cast<size_t>(i)]);
    return out;
  }();
  return luts[static_cast<size_t>(clampIndex(m))];
}

ColorF sampleRamp(std::span<const RampStop> stops, float t) noexcept {
  const size_t n = stops.size();
  size_t s = 0;
  while (s + 2 < n && t > stops[s + 1].t) ++s;
  const RampStop& a = stops[s];
  const RampStop& b = stops[s + 1];
  const float span = b.t - a.t;
  const float u = span > 0 ? std::clamp((t - a.t) / span, 0.0f, 1.0f) : 0.0f;
  return {a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u};
}

void buildLut(std::span<const RampStop> stops, ColorLut& out) noexcept {
  for (int i = 0; i < 256; ++i) {
    const ColorF c = sampleRamp(stops, static_cast<float>(i) / 255.0f);
    out[static_cast<size_t>(i)] = rgba(to8(c.r), to8(c.g), to8(c.b), 255);
  }
}

// --- Gradient --------------------------------------------------------------------------------------------

void Gradient::seed(Colormap preset) noexcept {
  const auto stops = colormapStops(preset);
  for (int i = 0; i < kStops; ++i) {
    const ColorF c = sampleRamp(stops, pos[i]);
    colors[i][0] = c.r;
    colors[i][1] = c.g;
    colors[i][2] = c.b;
  }
  rebuild();
}

void Gradient::rebuild() noexcept {
  RampStop s[kStops];
  for (int i = 0; i < kStops; ++i) s[i] = {pos[i], colors[i][0], colors[i][1], colors[i][2]};
  buildLut(s, lut);
}

void Gradient::setStop(int i, ColorF c, float position) noexcept {
  if (i < 0 || i >= kStops) return;
  colors[i][0] = std::clamp(c.r, 0.0f, 1.0f);
  colors[i][1] = std::clamp(c.g, 0.0f, 1.0f);
  colors[i][2] = std::clamp(c.b, 0.0f, 1.0f);
  if (i > 0 && i < kStops - 1) pos[i] = std::clamp(position, pos[i - 1] + 0.01f, pos[i + 1] - 0.01f);
  rebuild();
}

const ColorLut& Gradient::activeLut(int colormapIndex) const noexcept {
  return colormapIndex >= kCustomIndex ? lut : colormapLut(static_cast<Colormap>(std::max(0, colormapIndex)));
}

ColorF Gradient::sample(float t) const noexcept {
  RampStop s[kStops];
  for (int i = 0; i < kStops; ++i) s[i] = {pos[i], colors[i][0], colors[i][1], colors[i][2]};
  return sampleRamp(s, t);
}

// --- PaletteTrack ----------------------------------------------------------------------------------------

void PaletteTrack::Waypoint::resolve() noexcept {
  if (preset) {
    const auto stops = colormapStops(*preset);
    lut = colormapLut(*preset);
    // Bake the stops too, so a later "custom stops" edit has something honest to start from.
    for (int i = 0; i < Gradient::kStops; ++i) {
      const ColorF c = sampleRamp(stops, pos[i]);
      colors[i][0] = c.r;
      colors[i][1] = c.g;
      colors[i][2] = c.b;
    }
  } else {
    RampStop s[Gradient::kStops];
    for (int i = 0; i < Gradient::kStops; ++i) s[i] = {pos[i], colors[i][0], colors[i][1], colors[i][2]};
    buildLut(s, lut);
  }
}

PaletteTrack::PaletteTrack() {
  Waypoint a;
  a.t = 0.0f;
  a.preset = Colormap::Magma;
  a.resolve();
  Waypoint b;
  b.t = 1.0f;
  b.preset = Colormap::Viridis;
  b.resolve();
  points_.push_back(a);
  points_.push_back(b);
  rebuildNow();
}

void PaletteTrack::setEnabled(bool on) noexcept {
  if (on == enabled_) return;
  enabled_ = on;
  // Toggling swaps what activeLut returns between two tables whose contents rebuildNow never compares, so
  // rev-keyed caches must be told explicitly.
  ++rev_;
  rebuildNow();
}

void PaletteTrack::setPosition(float p) noexcept {
  pos_ = std::clamp(p, 0.0f, 1.0f);
  if (quantOf(pos_) != lastQuant_) rebuildNow();
}

bool PaletteTrack::setWaypoints(std::vector<Waypoint> points) {
  if (static_cast<int>(points.size()) < kMinPoints || static_cast<int>(points.size()) > kMaxPoints) return false;
  for (Waypoint& w : points) {
    w.t = std::clamp(w.t, 0.0f, 1.0f);
    w.resolve();
  }
  std::stable_sort(points.begin(), points.end(), [](const Waypoint& a, const Waypoint& b) { return a.t < b.t; });
  points_ = std::move(points);
  rebuildNow();
  return true;
}

void PaletteTrack::moveWaypoint(int i, float t) noexcept {
  const int n = static_cast<int>(points_.size());
  if (i < 0 || i >= n) return;
  float lo = i > 0 ? points_[static_cast<size_t>(i) - 1].t + 0.02f : 0.0f;
  const float hi = i + 1 < n ? points_[static_cast<size_t>(i) + 1].t - 0.02f : 1.0f;
  if (lo > hi) lo = hi;
  points_[static_cast<size_t>(i)].t = std::clamp(t, lo, hi);
  rebuildNow();
}

int PaletteTrack::insertWaypoint() {
  const int n = static_cast<int>(points_.size());
  if (n >= kMaxPoints) return -1;
  int gi = 0;
  float gw = 0.0f;
  for (int i = 0; i + 1 < n; ++i) {
    const float d = points_[static_cast<size_t>(i) + 1].t - points_[static_cast<size_t>(i)].t;
    if (d > gw) {
      gw = d;
      gi = i;
    }
  }
  Waypoint w;
  w.t = 0.5f * (points_[static_cast<size_t>(gi)].t + points_[static_cast<size_t>(gi) + 1].t);
  for (int s = 0; s < Gradient::kStops; ++s) {
    const Rgba c = evalColorAt(w.t, w.pos[s]);
    w.colors[s][0] = static_cast<float>(redOf(c)) / 255.0f;
    w.colors[s][1] = static_cast<float>(greenOf(c)) / 255.0f;
    w.colors[s][2] = static_cast<float>(blueOf(c)) / 255.0f;
  }
  w.resolve();
  points_.insert(points_.begin() + gi + 1, w);
  rebuildNow();
  return gi + 1;
}

bool PaletteTrack::removeWaypoint(int i) {
  if (static_cast<int>(points_.size()) <= kMinPoints || i < 0 || i >= static_cast<int>(points_.size())) return false;
  points_.erase(points_.begin() + i);
  rebuildNow();
  return true;
}

void PaletteTrack::setWaypointPreset(int i, Colormap m) noexcept {
  if (i < 0 || i >= static_cast<int>(points_.size())) return;
  points_[static_cast<size_t>(i)].preset = m;
  points_[static_cast<size_t>(i)].resolve();
  rebuildNow();
}

const ColorLut& PaletteTrack::activeLut(const Gradient& g, int colormapIndex) const noexcept {
  if (!enabled_ || points_.size() < 2) return g.activeLut(colormapIndex);
  return outLut_;
}

void PaletteTrack::bracket(float p, int* i0, float* u) const noexcept {
  const int n = static_cast<int>(points_.size());
  if (p <= points_.front().t) {
    *i0 = 0;
    *u = 0.0f;
    return;
  }
  if (p >= points_.back().t) {
    *i0 = n - 2;
    *u = 1.0f;
    return;
  }
  int s = 0;
  while (s < n - 2 && p > points_[static_cast<size_t>(s) + 1].t) ++s;
  const float span = points_[static_cast<size_t>(s) + 1].t - points_[static_cast<size_t>(s)].t;
  *i0 = s;
  *u = span > 1e-6f ? std::clamp((p - points_[static_cast<size_t>(s)].t) / span, 0.0f, 1.0f) : 0.0f;
}

Rgba PaletteTrack::evalColorAt(float p, float v) const noexcept {
  const int vi = std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255);
  if (points_.empty()) return rgba(255, 255, 255);
  if (points_.size() == 1) return points_.front().lut[static_cast<size_t>(vi)];
  int i0;
  float u;
  bracket(p, &i0, &u);
  return lerpOpaque(points_[static_cast<size_t>(i0)].lut[static_cast<size_t>(vi)],
                    points_[static_cast<size_t>(i0) + 1].lut[static_cast<size_t>(vi)], u);
}

int PaletteTrack::quantOf(float p) const noexcept {
  // Quantise the position clamped to the span the waypoints cover, so a sweep past either end does not keep
  // invalidating caches while the output sits still.
  const float pe = std::clamp(p, points_.front().t, points_.back().t);
  return static_cast<int>(std::lround(pe * static_cast<float>(kQuant - 1)));
}

void PaletteTrack::rebuildNow() noexcept {
  if (points_.size() >= 2) {
    int i0;
    float u;
    bracket(pos_, &i0, &u);
    const ColorLut& a = points_[static_cast<size_t>(i0)].lut;
    const ColorLut& b = points_[static_cast<size_t>(i0) + 1].lut;
    ColorLut next;
    for (size_t i = 0; i < 256; ++i) next[i] = lerpOpaque(a[i], b[i], u);
    // rev advances only when the table's content moved.
    if (std::memcmp(next.data(), outLut_.data(), sizeof(next)) != 0) {
      outLut_ = next;
      ++rev_;
    }
    lastQuant_ = quantOf(pos_);
  }
}

}  // namespace pq::ui
