#pragma once

// The project patch view's private helpers shared by canvas.cpp, view.cpp and lane_window.cpp (T5.x-one-circuit).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

// TL.1: band `b`'s lane in device px, snapped (dy: world units it is drawn shifted by: a drag growing a band above it):
// painting, hit tests and the playhead's damage agree on it.
inline ui::Rect laneBodyDevice(const ProjectCanvas& c, const ProjectPatch& pp, const Band& b, float dy = 0.0f) {
  const LaneRect r = laneBody(pp, b);
  return snapRect(c.deviceRect(r.x0, r.y0 + dy, r.x1 - r.x0, r.y1 - r.y0));
}

// TL.2: band `b`'s lane and its automation sub-lanes under it, one device rect (snapped as laneBodyDevice).
inline ui::Rect laneAreaDevice(const ProjectCanvas& c, const ProjectPatch& pp, const Band& b, float dy = 0.0f) {
  const LaneRect r = laneBody(pp, b);
  return snapRect(c.deviceRect(r.x0, r.y0 + dy, r.x1 - r.x0, r.y1 - r.y0 + b.subH));
}

// The playhead's column (device px, the line's left edge) in a lane showing [start, start + span).
inline float lanePlayheadX(const ui::Rect& body, double t, double start, double span) noexcept {
  const double u = (t - start) / (span > 0.0 ? span : 1.0);
  return std::floor(body.x0 + static_cast<float>(u * static_cast<double>(body.x1 - body.x0)));
}

// TL.1: every visible band lane of the view (lane_window.cpp): the lane's grid, clips with their names, MIDI notes (the
// arrangement's clip previews) and audio waveforms (the overview cache), the playhead; group / return / master lanes
// are empty strips (their automation: TL.2 sub-lanes under every lane, drawn here, their envelopes by the arrangement). Called from paintUnderlay with each band (under the blocks and cables).
// TX.lane-edit: with the arrangement next to the view, the clips are drawn by the arrangement's painters (selection
// outline, hover, audio handles, drag previews of a gesture in a lane), with the arrangement's time selection over the
// selected tracks' lanes and, in automation mode, their chosen envelopes.
void paintLanes(ui::PaintContext& ctx, const ProjectCanvas& canvas, ProjectPatchView& view);
// TL.1: the time ruler over the lanes — the arrangement's Ruler on the lanes' axis, a child of the canvas (lane_edit.cpp).
ui::Widget& addLaneRuler(ProjectCanvas& canvas, ProjectPatchView& view);

// TX.mixer-look: a track's mixer drawn as a face (mixer_face.cpp). A mixer node's SceneFace has the track as its owner
// and mixerFacePath as its path (the prefix, its size and its send pins: a new layout of the strip is a new face); the
// view's panelMaker makes the face for it.
inline constexpr std::string_view kMixerFacePrefix = "\x01mixer:";
// GU.3: the SceneFace path of a closed group drawn as its panel ("<prefix><w>x<h>").
inline constexpr std::string_view kGroupPanelFacePrefix = "\x01group-panel:";
// GU.5b: the SceneFace path of a value block drawn as its knob ("<prefix><w>x<h>"): a one-knob panel bound to it.
inline constexpr std::string_view kValueKnobFacePrefix = "\x01value-knob:";
struct MixerSend {
  doc::TrackId target;  // the return
  std::string letter;   // its letter (the pin's label)
};
std::string mixerFacePath(const Node& n);
std::unique_ptr<Face> makeMixerFace(ProjectPatchView& view, const ProjectPatch& pp, const Node& n);

// TX.lane-edit: the title strip of the clips in a window body `bodyH` device px high (0: the body is too low for one).
inline float laneClipTitleH(float bodyH, float uiScale) noexcept {
  return bodyH >= std::round(16.0f * uiScale) ? std::round(std::clamp(bodyH * 0.28f, 3.0f * uiScale, 14.0f * uiScale)) : 0.0f;
}

// TX.lane-edit: the band lanes as an edit surface of the arrangement's editor and automation (src/arrangement/
// edit_surface.h): its rows are the lanes with a clip row (MIDI and audio bands, in band order), all on one time axis
// (TL.1: the lanes' x range over the shared time window). Registered with the arrangement next to the view.
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
  // TL.2: the bands' automation sub-lanes (ProjectPatchView::subLanes) in device px, sorted top to bottom: y is the
  // window y of a lane's top (laneRect(i).y == laneRows()[i].y), row its band's row (−1: a group / return / master).
  // Recomputed when the model, the canvas's view or rect, or a drag's band shift changed; a band dragged by its title
  // (lifted) has none meanwhile.
  std::span<const AutomationLaneRow> laneRows() const override;
  ui::IRect laneRect(size_t i) const override;
  void damageAll() override;
  std::vector<ui::MenuItem> menuItems() override;

  // The band of a row / the row of a band (−1: none); the lane of a row (device px, snapped).
  uint32_t bandOf(int32_t row) const;
  int32_t rowOfBand(uint32_t band) const;
  ui::Rect body(int32_t row) const;
  // The row a right-click menu was opened on (its automation entries).
  int32_t menuRow = -1;

private:
  void sync() const;
  void syncLanes() const;
  ProjectPatchView& view_;
  // TL.2: laneRows' cache and what it was computed at.
  mutable std::vector<AutomationLaneRow> lanes_;
  mutable int32_t laneX_ = 0, laneW_ = 0;  // every lane's x range (device px)
  mutable std::vector<double> laneKey_;    // view x, y, zoom, rect, scale, rebuilds, then each band's drag shift (lifted: 1e300)
  mutable doc::Snapshot laneDoc_;          // the document lanes_' lane pointers point into
  mutable doc::Snapshot doc_;          // what rows_ point into
  mutable uint64_t built_ = ~uint64_t{0};  // the view's rebuild count rows_ were made at
  mutable std::vector<ArrangementRow> rows_;
  mutable std::vector<uint32_t> bands_;    // per row
  mutable std::vector<int32_t> rowOfBand_;  // per band
};

}  // namespace pq::views::project_patch
