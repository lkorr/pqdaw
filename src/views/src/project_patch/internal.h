#pragma once

// The project patch view's private helpers shared by canvas.cpp, view.cpp and lane_window.cpp (T5.x-one-circuit).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "../arrangement/edit_surface.h"
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
// TX.lane-edit: with the arrangement next to the view, the clips are drawn by the arrangement's painters (selection
// outline, hover, audio handles, drag previews of a gesture in a window), with the arrangement's time selection over the
// selected tracks' windows and, in automation mode, their chosen envelopes.
void paintLaneWindows(ui::PaintContext& ctx, const ProjectCanvas& canvas, ProjectPatchView& view);

// TX.lane-edit: the title strip of the clips in a window body `bodyH` device px high (0: the body is too low for one).
inline float laneClipTitleH(float bodyH, float uiScale) noexcept {
  return bodyH >= std::round(16.0f * uiScale) ? std::round(std::clamp(bodyH * 0.28f, 3.0f * uiScale, 14.0f * uiScale)) : 0.0f;
}

// TX.lane-edit: the lane windows as an edit surface of the arrangement's editor and automation (src/arrangement/
// edit_surface.h): its rows are the windows (bands with a window, in band order), each with its own time axis (the
// window body's x span over [laneStart, laneStart + laneSpan)). Registered with the arrangement next to the view.
class LaneEditSurface final : public arrangement::EditSurface {
public:
  explicit LaneEditSurface(ProjectPatchView& view) : view_(view) {}
  ui::Widget& widget() const override;
  bool active() const override;
  std::span<const ArrangementRow> rows() const override;
  int32_t rowAt(float deviceY) const override;
  int32_t clampRow(float deviceY) const override;
  ui::IRect rowLaneRect(int32_t row) const override;
  ui::IRect viewport() const override;
  arrangement::TimeAxis axis(int32_t row) const override;
  double gridPxPerTick() const override;
  int32_t clipTitleHeight(int32_t row) const override;
  void revealTick(Ticks t) override;
  void damageAll() override;
  std::vector<ui::MenuItem> menuItems() override;

  // The band of a row / the row of a band (−1: none); the window body of a row (device px, snapped).
  uint32_t bandOf(int32_t row) const;
  int32_t rowOfBand(uint32_t band) const;
  ui::Rect body(int32_t row) const;
  // The row a right-click menu was opened on (its automation entries).
  int32_t menuRow = -1;

private:
  void sync() const;
  ProjectPatchView& view_;
  mutable doc::Snapshot doc_;          // what rows_ point into
  mutable uint64_t built_ = ~uint64_t{0};  // the view's rebuild count rows_ were made at
  mutable std::vector<ArrangementRow> rows_;
  mutable std::vector<uint32_t> bands_;    // per row
  mutable std::vector<int32_t> rowOfBand_;  // per band
};

}  // namespace pq::views::project_patch
