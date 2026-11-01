#pragma once

// The project patch view's private helpers shared by canvas.cpp, view.cpp and lane_window.cpp (T5.x-one-circuit).

#include <cmath>
#include <cstdint>

#include "pq/core/time.h"
#include "pq/ui/widget.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

inline ui::Rect snapRect(ui::Rect r) noexcept { return {std::round(r.x0), std::round(r.y0), std::round(r.x1), std::round(r.y1)}; }

inline ui::IRect toIRect(const ui::Rect& r) noexcept {
  const auto x0 = static_cast<int32_t>(std::floor(r.x0)), y0 = static_cast<int32_t>(std::floor(r.y0));
  return {x0, y0, static_cast<int32_t>(std::ceil(r.x1)) - x0, static_cast<int32_t>(std::ceil(r.y1)) - y0};
}

inline bool overlaps(const ui::Rect& r, const ui::IRect& clip) noexcept {
  return r.x1 >= static_cast<float>(clip.x) && r.x0 <= static_cast<float>(clip.right()) && r.y1 >= static_cast<float>(clip.y) &&
         r.y0 <= static_cast<float>(clip.bottom());
}

// A lane window's body in device px, snapped: painting and the playhead's damage agree on it.
inline ui::Rect laneBodyDevice(const ProjectCanvas& c, const Node& n) {
  const LaneRect r = laneBody(n);
  return snapRect(c.deviceRect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0));
}

// The playhead's column (device px, the line's left edge) in a window body showing [start, start + span).
inline float lanePlayheadX(const ui::Rect& body, Ticks t, Ticks start, Ticks span) noexcept {
  const double u = static_cast<double>(t - start) / static_cast<double>(span > 0 ? span : 1);
  return std::floor(body.x0 + static_cast<float>(u * static_cast<double>(body.x1 - body.x0)));
}

// Every visible lane window of the view (lane_window.cpp): the lane's grid, clips with their names, MIDI notes (the
// arrangement's clip previews) and audio waveforms (the overview cache), the playhead. Called from paintOverlay.
void paintLaneWindows(ui::PaintContext& ctx, const ProjectCanvas& canvas, ProjectPatchView& view);

}  // namespace pq::views::project_patch
