#pragma once

// Colour ramps for signal displays (SPEC §11.1), ported from PQ Meter:
//   - the ten preset colormaps, sampleRamp and buildLutFromStops   ← src/ui/theme.h (Theme::rampStops etc.)
//   - Gradient: six user-movable stops baked into a 256-entry LUT  ← src/ui/gradient.h (struct Gradient)
//   - PaletteTrack: interpolation between whole colormaps          ← src/ui/palette_track.h
// The ImGui settings rows, JSON persistence, the saved-colormap store and the LFO of the originals are UI /
// settings concerns that arrive with the widget toolkit and the modulation system; what is here is the
// colour maths those rows edit, with PQ Meter's numbers unchanged.
//
// Not thread-safe (UI / analysis thread objects). Nothing here is on the audio thread.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "pq/ui/color.h"
#include "pq/ui/theme.h"

namespace pq::ui {

using signal_theme::RampStop;
using ColorLut = std::array<Rgba, 256>;

// APPEND-ONLY, like PQ Meter's list: persisted indices must keep their meaning.
enum class Colormap : uint8_t { Magma, Inferno, Viridis, Grayscale, Ice, Gold, Silver, Pastel, Neon, Copper };
inline constexpr int kColormapCount = 10;

std::string_view colormapName(Colormap m) noexcept;
std::span<const RampStop> colormapStops(Colormap m) noexcept;
// The preset's LUT (built once on first use; thread-safe static initialisation).
const ColorLut& colormapLut(Colormap m) noexcept;

struct ColorF {
  float r, g, b;
};

// Piecewise-linear interpolation of a ramp at t (Theme::sampleRamp). Stops must be ordered by t; n >= 2.
ColorF sampleRamp(std::span<const RampStop> stops, float t) noexcept;
// 256 entries, entry i = sampleRamp(i / 255), alpha 255 (Theme::buildLutFromStops).
void buildLut(std::span<const RampStop> stops, ColorLut& out) noexcept;

// LUT entry for t in [0, 1] (nearest, as PQ Meter's paletteAt / PaletteTrack::sample read it).
inline Rgba lutAt(const ColorLut& lut, float t) noexcept {
  const float f = t * 255.0f + 0.5f;
  const int i = f <= 0.0f ? 0 : (f >= 255.0f ? 255 : static_cast<int>(f));
  return lut[static_cast<size_t>(i)];
}

// A user-editable ramp: six (colour, position) stops, the ends pinned at 0 and 1, the middle four movable.
// The default positions put the last movable stop at 90 % so the loud-to-peak range gets its own colour.
struct Gradient {
  static constexpr int kStops = 6;
  // A module's colormap index meaning "this gradient" (one past the last preset), as in PQ Meter.
  static constexpr int kCustomIndex = kColormapCount;

  float colors[kStops][3] = {};
  float pos[kStops] = {0.0f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};
  ColorLut lut{};

  Gradient() noexcept { seed(Colormap::Magma); }

  // Samples a preset into the stops at their current positions, then rebuilds the LUT.
  void seed(Colormap preset) noexcept;
  void rebuild() noexcept;
  // Sets stop i's colour and (for the movable stops 1..4) position; the position yields to its neighbours
  // by 0.01 so the stops stay ordered (PQ Meter's drawStopRows rule). Rebuilds the LUT.
  void setStop(int i, ColorF c, float position) noexcept;
  // The LUT to paint through for a module's colormap index: this gradient for kCustomIndex, else a preset.
  const ColorLut& activeLut(int colormapIndex) const noexcept;
  // Interpolated colour at t in [0, 1] (float, not via the LUT).
  ColorF sample(float t) const noexcept;
};

// Interpolation between whole colormaps along a 0..1 track (PQ Meter's PaletteTrack). Waypoints pin a
// colormap at a position; a track position resolves to the blend of the two bracketing waypoints' LUTs,
// entry by entry (blending the resolved tables blends the functions, whatever their stop layouts), clamped
// to the nearest waypoint past either end. The position is quantised to kQuant steps before rebuilding,
// and rev() only advances when the resolved table's content changes, so caches keyed on it (retonemapped
// spectrogram history) invalidate a handful of times per second under a slow sweep, not every frame.
//
// In PQ Meter the position is a base value plus a one-slot LFO; here the caller supplies the effective
// position (the DAW's modulation system drives it).
class PaletteTrack {
public:
  static constexpr int kMinPoints = 2, kMaxPoints = 8;
  static constexpr int kQuant = 512;

  struct Waypoint {
    float t = 0.0f;
    // A built-in ramp (re-resolved from the preset so it stays bit-exact), or nullopt for the stops below.
    std::optional<Colormap> preset;
    float colors[Gradient::kStops][3] = {};
    float pos[Gradient::kStops] = {0.0f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};
    ColorLut lut{};
    void resolve() noexcept;
  };

  PaletteTrack();  // Magma at 0, Viridis at 1 (PQ Meter's default pair)

  bool enabled() const noexcept { return enabled_; }
  void setEnabled(bool on) noexcept;
  uint32_t rev() const noexcept { return rev_; }
  float position() const noexcept { return pos_; }
  // Sets the effective position and rebuilds the table if its quantised value moved.
  void setPosition(float p) noexcept;

  const std::vector<Waypoint>& waypoints() const noexcept { return points_; }
  // Replaces the waypoints (2..8, sorted by t here). Returns false and changes nothing if the count is out
  // of range.
  bool setWaypoints(std::vector<Waypoint> points);
  // Moves waypoint i, keeping a 0.02 gap to its neighbours (the drag rule).
  void moveWaypoint(int i, float t) noexcept;
  // Inserts a waypoint at the midpoint of the widest gap, seeded with the blend already there (so adding is
  // invisible until it is reassigned). Returns its index, or -1 at kMaxPoints.
  int insertWaypoint();
  // Removes waypoint i unless that would leave fewer than kMinPoints. Returns whether it did.
  bool removeWaypoint(int i);
  // Assigns a preset to waypoint i.
  void setWaypointPreset(int i, Colormap m) noexcept;

  // The table to paint through: the blended map while enabled, else the gradient / preset pair.
  const ColorLut& activeLut(const Gradient& g, int colormapIndex) const noexcept;
  // Blended colour at track position p, sampled at value v (the strip preview).
  Rgba evalColorAt(float p, float v) const noexcept;

private:
  void bracket(float p, int* i0, float* u) const noexcept;
  int quantOf(float p) const noexcept;
  void rebuildNow() noexcept;

  bool enabled_ = false;
  float pos_ = 0.0f;
  std::vector<Waypoint> points_;
  ColorLut outLut_{};
  uint32_t rev_ = 1;
  int lastQuant_ = -1;
};

}  // namespace pq::ui
