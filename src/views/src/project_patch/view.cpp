// The project patch view (T4.13): rebuilding the model on document changes, the morph from / to the arrangement, the
// semantic-zoom animations, the edits and the menus. The canvas (gestures, painting) is canvas.cpp. See
// pq/views/project_patch/project_patch_view.h.

#include <algorithm>
#include <numbers>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include "../common/paint.h"
#include "../common/panel_template.h"
#include "internal.h"
#include <cstdio>
#include <unordered_map>
#include "pq/views/browser/library_load.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/views/browser/library.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/device_circuit.h"
#include "pq/views/devices/knob_curve_menu.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/panel/panel_window.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Vec2;

namespace {

ArrangementView* arrangementNear(const ui::Widget& self) {
  const ui::Widget* slot = self.parent();
  const ui::Widget* area = slot ? slot->parent() : nullptr;
  return area ? dynamic_cast<ArrangementView*>(area->find("arrangement")) : nullptr;
}

const graph::PatchLibrary& emptyLibrary() {
  static const graph::PatchLibrary lib;
  return lib;
}

float smooth(float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

Rect lerpRect(const Rect& a, const Rect& b, float t) noexcept {
  return {a.x0 + (b.x0 - a.x0) * t, a.y0 + (b.y0 - a.y0) * t, a.x1 + (b.x1 - a.x1) * t, a.y1 + (b.y1 - a.y1) * t};
}

}  // namespace

ProjectPatchView::ProjectPatchView(ViewContext& ctx) : Widget("project-patch"), ctx_(ctx) {
  setLayout(ui::LayoutKind::Custom);
  setClipChildren(true);
  canvas_ = &add<ProjectCanvas>(*this);
  // TX.cables: the cables glow with their signal (polled by the canvas only while a source is live).
  canvas_->cableLevels = [this](std::span<const uint32_t> wires, std::span<float> levels) { cableLevels(wires, levels); };
  canvas_->cableSignalLive = [this] { return cableSignalLive(); };
  docSub_ = ctx_.subscribe([this](const doc::StoreEvent& e) { onDocument(e); });
  viewSub_ = ctx_.observe([this](uint32_t bits) {
    if (bits & view_change::kTransport) refreshTransport();
    if ((bits & view_change::kWaveforms) && shown()) damageLanes();  // an overview arrived or grew: the windows only
    if (bits & view_change::kAnalysis) wakeFaces();  // TX.spectrum: a Spectrum face's tap appeared
  });
}

ProjectPatchView::ProjectPatchView(ViewContext& ctx, doc::DeviceId scope) : ProjectPatchView(ctx) {
  scope_ = scope;  // TX.groups: the Device view's group panel (the constructor above bound nothing global)
}

ProjectPatchView::ProjectPatchView(ViewContext& ctx, FreeRack) : ProjectPatchView(ctx) {
  freeRack_ = true;  // TX.free-rack: the Device view's free-device rack (setRackTrack picks the track)
  // TMM.13-fix2 (MM review "UI and aesthetic" 9): it reads from the left, as the chain under it (centred, its left
  // 300 px stayed empty).
  canvas_->setFitAlignLeft(true);
}

void ProjectPatchView::setRackTrack(doc::TrackId track) {
  if (!freeRack_ || track == rackTrack_) return;
  rackTrack_ = track;
  stale_ = true;
  fitted_ = false;  // another track's devices: fit them
  invalidateArrange();
  markDirty();
}

ProjectPatchView::~ProjectPatchView() {
  laneAbort();  // TX.lane-edit: no drag of the arrangement's editor may outlive the windows' surface
  if (valueGesture_ && ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
  if (embedded()) return;  // TX.groups / TX.free-rack: an embedded view bound no actions of the main one
  ctx_.clearViewAction("view.togglePatch");
  ctx_.clearViewAction("view.trackCircuit");
  ctx_.setCircuitOpener({});
}

// ---- shown and the morph --------------------------------------------------------------------------------------------

bool ProjectPatchView::shown() const noexcept {
  if (embedded()) {  // TX.free-rack: an embedded view is shown while every ancestor is (the detail area, its mode)
    for (const Widget* w = this; w != nullptr; w = w->parent())
      if (!w->visible()) return false;
    return tree() != nullptr;
  }
  const Widget* slot = parent();
  return slot && slot->visible() && visible();
}

void ProjectPatchView::toggle() {
  if (morph_.active) {  // reverse the running morph
    beginMorph(!morph_.in);
    return;
  }
  setShown(!shown(), true);
}

void ProjectPatchView::setShown(bool on, bool animate) {
  Widget* slot = parent();
  if (slot == nullptr) return;
  if (!animate && morph_.active) {
    morph_.active = false;
    canvas_->setVisible(true);
  }
  if (on == shown() && !morph_.active) return;
  ArrangementView* arr = arrangementNear(*this);
  if (on) {
    if (hideOthers) hideOthers();
    slot->setVisible(true);
    initLaneTime();  // T5.x-one-circuit: the windows start where the arrangement is
    (void)laneEditReady();  // TX.lane-edit: the windows get the arrangement's selection damage from now on
    stale_ = stale_ || built_ != ctx_.doc();
    sync();
    if (arr != nullptr && tree() != nullptr) {
      tree()->layoutNow();  // the arrangement's rows, the canvas's rect
      canvas_->setView(viewForArrangement());
      fitted_ = true;
    }
    if (animate && arr != nullptr && tree() != nullptr) {
      captureArrangement();
      beginMorph(true);
    } else {
      canvas_->setVisible(true);
    }
    if (arr) arr->setVisible(false);
    if (tree()) tree()->setFocus(canvas_);
    if (costSource_ != nullptr && showCosts_) {  // the labels measure only while shown
      costSource_->setCostMetering(true);
      pollCosts();
    }
    refreshTransport();
  } else {
    if (animate && arr != nullptr && tree() != nullptr) {
      captureArrangement();
      beginMorph(false);
      return;  // hidden when the morph ends
    }
    slot->setVisible(false);
    if (costSource_ != nullptr && showCosts_) costSource_->setCostMetering(false);
    refreshTransport();  // hidden: the playhead timer stops
    if (arr) {
      arr->setVisible(true);
      if (tree()) tree()->setFocus(arr);
    }
  }
  markDirty();
}

ui::Rect ProjectPatchView::morphBandRect(uint32_t band) const noexcept {
  if (!morph_.active || band >= morph_.bands.size()) return {};
  return lerpRect(morph_.bands[band].lane, morph_.bands[band].band, smooth(morph_.t));
}

void ProjectPatchView::beginMorph(bool in) {
  const float t = morph_.active ? morph_.t : (in ? 0.0f : 1.0f);
  morph_.active = true;
  morph_.in = in;
  morph_.start = -1.0;
  morph_.t = t;
  // Resume from where a reversed morph was: the start time is set at the next frame, shifted by the progress made.
  morph_.resumeFrom = in ? t : 1.0f - t;
  canvas_->setVisible(false);
  if (ArrangementView* arr = arrangementNear(*this)) arr->setVisible(false);
  requestFrame();
  markDirty();
}

void ProjectPatchView::finishMorph() {
  morph_.active = false;
  if (morph_.in) {
    morph_.t = 1.0f;
    canvas_->setVisible(true);
    if (tree()) tree()->setFocus(canvas_);
  } else {
    morph_.t = 0.0f;
    canvas_->setVisible(true);
    if (Widget* slot = parent()) slot->setVisible(false);
    if (costSource_ != nullptr && showCosts_) costSource_->setCostMetering(false);
    refreshTransport();
    if (ArrangementView* arr = arrangementNear(*this)) {
      arr->setVisible(true);
      if (tree()) tree()->setFocus(arr);
    }
  }
  markDirty();
}

// Where the canvas starts so that the bands line up with the lanes they unfold from: a zoom giving bands about the
// lanes' height, and the first visible lane's band at that lane's top.
circuit::CircuitCanvas::View ProjectPatchView::viewForArrangement() const {
  circuit::CircuitCanvas::View v;
  const ArrangementView* arr = arrangementNear(*this);
  const float us = tree() ? tree()->scale() : 1.0f;
  const ui::IRect area = parent() ? parent()->rect() : rect();
  if (arr == nullptr || patch_.bands.empty()) return canvas_->view();
  // The median lane and band heights.
  std::vector<float> lanes, bands;
  for (size_t i = 0; i < arr->rows().size(); ++i) lanes.push_back(static_cast<float>(arr->rows()[i].h) / us);
  for (const Band& b : patch_.bands) bands.push_back(b.h + metrics::kBandGap);
  if (lanes.empty()) {
    v.zoom = 0.4;
  } else {
    std::nth_element(lanes.begin(), lanes.begin() + static_cast<std::ptrdiff_t>(lanes.size() / 2), lanes.end());
    std::nth_element(bands.begin(), bands.begin() + static_cast<std::ptrdiff_t>(bands.size() / 2), bands.end());
    v.zoom = std::clamp(static_cast<double>(lanes[lanes.size() / 2]) / std::max(1.0f, bands[bands.size() / 2]), 0.12, 1.0);
  }
  v.x = static_cast<double>(patch_.x0) - 12.0;
  v.y = static_cast<double>(patch_.y0) - 8.0;
  const ui::IRect vp = arr->lanesViewport();
  for (size_t i = 0; i < arr->rows().size(); ++i) {
    const ui::IRect lr = arr->rowLaneRect(static_cast<int32_t>(i));
    if (lr.bottom() <= vp.y) continue;
    const uint32_t b = patch_.bandOf(arr->rows()[i].id);
    if (b == kNone) break;
    const double offset = (lr.y - area.y) / static_cast<double>(us);  // logical px from the top of the view
    v.y = static_cast<double>(patch_.bands[b].y) - offset / v.zoom;
    break;
  }
  return v;
}

void ProjectPatchView::captureArrangement() {
  morph_.bands.assign(patch_.bands.size(), {});
  morph_.clips.clear();
  morph_.nodes.clear();
  ArrangementView* arr = arrangementNear(*this);
  const ui::IRect area = parent() ? parent()->rect() : rect();
  const float us = tree() ? tree()->scale() : 1.0f;
  // The bands on the canvas (its view now: set for a morph in, the user's for a morph out).
  const circuit::CircuitCanvas::View cv = canvas_->view();
  const float k = static_cast<float>(cv.zoom) * us;
  auto dev = [&](float wx, float wy) {
    return Vec2{static_cast<float>(area.x) + static_cast<float>((wx - cv.x) * k), static_cast<float>(area.y) + static_cast<float>((wy - cv.y) * k)};
  };
  auto devRect = [&](float x, float y, float w, float h) {
    const Vec2 a = dev(x, y), b = dev(x + w, y + h);
    return Rect{a.x, a.y, b.x, b.y};
  };
  std::vector<int32_t> laneOf(patch_.bands.size(), -1);  // −2: pinned row
  std::vector<Rect> lane(patch_.bands.size());
  bool any = false;
  if (arr != nullptr) {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i) {
      const uint32_t b = patch_.bandOf(rows[i].id);
      if (b == kNone) continue;
      const ui::IRect r = arr->rowLaneRect(static_cast<int32_t>(i));
      lane[b] = {static_cast<float>(area.x), static_cast<float>(r.y), static_cast<float>(area.right()), static_cast<float>(r.bottom())};
      laneOf[b] = static_cast<int32_t>(i);
      any = true;
    }
    const auto bottom = arr->bottomRows();
    for (size_t i = 0; i < bottom.size(); ++i) {
      const uint32_t b = patch_.bandOf(bottom[i].id);
      if (b == kNone) continue;
      const ui::IRect r = arr->pinnedRowRect(i);
      lane[b] = {static_cast<float>(area.x), static_cast<float>(r.y), static_cast<float>(area.right()), static_cast<float>(r.bottom())};
      laneOf[b] = -2;
      any = true;
    }
  }
  for (uint32_t b = 0; b < patch_.bands.size(); ++b) {
    const Band& band = patch_.bands[b];
    Rect lr;
    if (laneOf[b] != -1) {
      lr = lane[b];
    } else {  // a member of a folded group: from the bottom of the nearest shown ancestor's lane
      int32_t p = band.parent;
      while (p >= 0 && laneOf[static_cast<size_t>(p)] == -1) p = patch_.bands[static_cast<size_t>(p)].parent;
      const Rect pr = p >= 0 ? lane[static_cast<size_t>(p)] : Rect{static_cast<float>(area.x), static_cast<float>(area.bottom()),
                                                                     static_cast<float>(area.right()), static_cast<float>(area.bottom())};
      lr = {pr.x0, pr.y1, pr.x1, pr.y1};
    }
    morph_.bands[b].lane = lr;
    morph_.bands[b].band = devRect(band.x0, band.y, band.x1 - band.x0, band.h);
  }
  (void)any;
  // Clips: those the arrangement shows collapse into their source's strip.
  if (arr != nullptr) {
    const ui::IRect vp = arr->lanesViewport();
    const doc::Project& p = *ctx_.doc();
    const double t0 = static_cast<double>(laneStart_), span = static_cast<double>(std::max<Ticks>(laneSpan_, 1));
    for (uint32_t b = 0; b < patch_.bands.size() && morph_.clips.size() < 1024; ++b) {
      const Band& band = patch_.bands[b];
      if ((band.kind != doc::TrackKind::Midi && band.kind != doc::TrackKind::Audio) || laneOf[b] < 0) continue;
      const doc::Track* t = doc::findTrackPtr(p, band.track);
      if (t == nullptr) continue;
      for (const doc::ClipSlot& c : t->clips) {
        const ui::IRect r = arr->clipRect({band.track, c.id});
        const ui::IRect v = ui::intersect(r, vp);
        if (v.empty()) continue;
        Morph::ClipGeom g;
        g.lane = v.toRect();
        g.band = b;
        // T5.x-one-circuit: into the lane window's time window (clips outside it shrink onto its edges).
        g.u0 = static_cast<float>(std::clamp((static_cast<double>(c.start()) - t0) / span, 0.0, 1.0));
        g.u1 = static_cast<float>(std::clamp((static_cast<double>(c.end()) - t0) / span, 0.0, 1.0));
        g.colour = c.colour().rgb;
        morph_.clips.push_back(g);
        if (morph_.clips.size() >= 1024) break;
      }
    }
  }
  for (uint32_t n = 0; n < patch_.nodes.size(); ++n) {
    const Node& nd = patch_.nodes[n];
    morph_.nodes.push_back({devRect(nd.x, nd.y, nd.w, nd.h), nd.band, n});
  }
}

void ProjectPatchView::onFrame(double now) {
  // GU.5: windows whose group went (hidden at once by syncWindows) are removed here, outside any event of theirs.
  if (windowsStale_) {
    windowsStale_ = false;
    const doc::Snapshot& snap = ctx_.doc();
    for (size_t i = windows_.size(); i-- > 0;) {
      panel::PanelWindow* w = windows_[i];
      if (snap && doc::findDevice(*snap, w->group()).device != nullptr) continue;
      windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
      (void)removeChild(*w);
    }
  }
  // TX.node-menu: the push-apart after a spawn steps with the frame clock until the blocks are apart and at rest.
  if (pushing()) {
    if (push_.last < 0.0) push_.last = now - 1.0 / 60.0;
    const double dt = std::clamp(now - push_.last, 0.0, 0.05);
    push_.last = now;
    const bool moving = stepPush(dt);
    applyPush();
    ++push_.frames;
    if (moving && push_.frames < 240) {
      requestFrame();
    } else {
      push_.bodies.clear();
      if (cascade_ < kPushCascade) {  // TX.block-drag: what the push grew into, next
        ++cascade_;
        (void)startPush();
      }
    }
  }
  if (morph_.active) {
    if (morph_.start < 0.0) morph_.start = now - static_cast<double>(morph_.resumeFrom) * kMorphSeconds;
    const float p = static_cast<float>(std::clamp((now - morph_.start) / kMorphSeconds, 0.0, 1.0));
    morph_.t = morph_.in ? p : 1.0f - p;
    ++stats_.morphFrames;
    markDirty();
    if (p >= 1.0f) finishMorph();
    else requestFrame();
  }
  if (viewAnim_.active) {
    if (viewAnim_.start < 0.0) viewAnim_.start = now;
    const double p = std::clamp((now - viewAnim_.start) / kViewAnimSeconds, 0.0, 1.0);
    const double e = static_cast<double>(smooth(static_cast<float>(p)));
    // Zoom interpolated geometrically, the centre linearly in world units.
    const double z = viewAnim_.from.zoom * std::pow(viewAnim_.to.zoom / viewAnim_.from.zoom, e);
    const ui::IRect r = canvas_->rect();
    const double us = tree() ? tree()->scale() : 1.0;
    const double hw = r.w / us * 0.5, hh = r.h / us * 0.5;
    const double cx0 = viewAnim_.from.x + hw / viewAnim_.from.zoom, cy0 = viewAnim_.from.y + hh / viewAnim_.from.zoom;
    const double cx1 = viewAnim_.to.x + hw / viewAnim_.to.zoom, cy1 = viewAnim_.to.y + hh / viewAnim_.to.zoom;
    const double cx = cx0 + (cx1 - cx0) * e, cy = cy0 + (cy1 - cy0) * e;
    circuit::CircuitCanvas::View v;
    v.zoom = z;
    v.x = cx - hw / z;
    v.y = cy - hh / z;
    canvas_->setView(p >= 1.0 ? viewAnim_.to : v);
    if (p >= 1.0) viewAnim_.active = false;
    else requestFrame();
  }
}

void ProjectPatchView::arrange(ui::LayoutPass& pass) {
  pass.place(*canvas_, rect());
  // GU.5: the panel windows over the canvas, where their view state puts them (logical px from the view's top-left),
  // kept so their title bars stay reachable; none while the view morphs.
  for (panel::PanelWindow* w : windows_) {
    if (!w->visible()) continue;
    const doc::PanelWindowView& st = w->state();
    if (morph_.active || rect().w <= 0 || rect().h <= 0) {
      pass.place(*w, {});
      continue;
    }
    const int32_t ww = std::min(rect().w, pass.px(std::max(st.w, panel::PanelWindow::kMinW)));
    const int32_t hh = st.minimised ? pass.px(panel::PanelWindow::kTitleH) : std::min(rect().h, pass.px(std::max(st.h, panel::PanelWindow::kMinH)));
    const int32_t keep = pass.px(48.0f);
    const int32_t x = std::clamp(rect().x + pass.px(st.x), rect().x - ww + keep, rect().right() - keep);
    const int32_t y = std::clamp(rect().y + pass.px(st.y), rect().y, rect().bottom() - pass.px(panel::PanelWindow::kTitleH));
    pass.place(*w, {x, y, ww, hh});
  }
  // TX.groups: an embedded view (a group's panel) builds itself once it has a size (it fits the capsule then).
  if (embedded() && rect().w > 0 && rect().h > 0 && (stale_ || built_ != ctx_.doc())) sync();
  // TX.free-rack: the rack's height was dragged: its contents fit again, unless the user moved the view since.
  if (freeRack_ && fitted_ && rect().w > 0 && rect().h > 0 && !(rect() == fitRect_)) {
    const circuit::CircuitCanvas::View v = canvas_->view();
    if (v.x == fitView_.x && v.y == fitView_.y && v.zoom == fitView_.zoom) fitEmbedded();
  }
}

void ProjectPatchView::paint(ui::PaintContext& ctx) {
  if (!morph_.active) return;  // the canvas paints the patch
  const auto t0 = std::chrono::steady_clock::now();
  const ui::Theme* th = ctx.res.theme;
  if (th == nullptr) return;
  ui::DrawList& dl = ctx.dl;
  const float e = smooth(morph_.t);
  dl.pushClip(rect().toRect());
  dl.addRectFilled(ui::intersect(rect(), ctx.clip).toRect(), ui::lerp(th->arrangement.background, th->circuit.background, e));
  const doc::Project& p = *ctx_.doc();
  // Bands: from the lane rect to the band rect.
  std::vector<Rect> now(morph_.bands.size());
  for (size_t b = 0; b < morph_.bands.size() && b < patch_.bands.size(); ++b) {
    const Band& band = patch_.bands[b];
    const Rect r = lerpRect(morph_.bands[b].lane, morph_.bands[b].band, e);
    now[b] = r;
    const ui::Rgba col = ui::hex(band.colour);
    const ui::Rgba laneCol = th->arrangement.lane;
    const ui::Rgba bandCol = ui::lerp(th->circuit.background, col, band.active ? 0.14f : 0.06f);
    if (r.y1 - r.y0 >= 0.5f) {
      dl.addRectFilled(r, ui::lerp(laneCol, bandCol, e));
      dl.addRectFilled({r.x0, r.y0, r.x0 + std::max(2.0f, std::round(3.0f * ctx.scale.factor)), r.y1}, ui::mulAlpha(col, e));
      dl.addHLine(r.x0, r.x1, r.y1 - 0.5f, ui::mulAlpha(th->arrangement.separator, 1.0f - e), 1.0f);
    }
    if (e > 0.5f && ctx.res.uiFonts && r.y1 - r.y0 > 12.0f) {
      const ui::IRect tr{static_cast<int32_t>(r.x0) + 8, static_cast<int32_t>(r.y0) + 2, static_cast<int32_t>(r.x1 - r.x0) - 16,
                         static_cast<int32_t>(std::min(16.0f * ctx.scale.factor, r.y1 - r.y0 - 2.0f))};
      paint::text(ctx, ctx.res.uiFonts->small, tr, band.name, ui::mulAlpha(th->circuit.symbolText, (e - 0.5f) * 2.0f), ui::TextAlign::Left);
    }
  }
  // Nodes fade in, moving with their band.
  const float na = std::clamp((e - 0.3f) / 0.7f, 0.0f, 1.0f);
  if (na > 0.0f)
    for (const Morph::NodeGeom& g : morph_.nodes) {
      if (g.band >= now.size()) continue;
      const Rect& br = morph_.bands[g.band].band;
      const float dx = now[g.band].x0 - br.x0, dy = now[g.band].y0 - br.y0;
      const Rect r{g.end.x0 + dx, g.end.y0 + dy, g.end.x1 + dx, g.end.y1 + dy};
      if (r.x1 < static_cast<float>(ctx.clip.x) || r.x0 > static_cast<float>(ctx.clip.right()) || r.y1 < static_cast<float>(ctx.clip.y) ||
          r.y0 > static_cast<float>(ctx.clip.bottom()))
        continue;
      const bool source = patch_.nodes[g.node].kind == NodeKind::Source;
      const float a = source ? e : na;
      dl.addRectFilled(r, ui::mulAlpha(th->circuit.symbolFill, a));
      dl.addRectStroke(r, ui::mulAlpha(th->circuit.symbolStroke, a), 1.0f);
    }
  // Clips: from their place in the lane into their source node's strip.
  for (const Morph::ClipGeom& c : morph_.clips) {
    if (c.band >= now.size()) continue;
    const Band& band = patch_.bands[c.band];
    if (band.source == kNone) continue;
    const Node& src = patch_.nodes[band.source];
    const Rect& br = morph_.bands[c.band].band;
    const float dx = now[c.band].x0 - br.x0, dy = now[c.band].y0 - br.y0;
    const Rect sr = morph_.nodes[band.source].end;
    const float k = (sr.x1 - sr.x0) / std::max(1.0f, src.w);
    const LaneRect lb = laneBody(src);  // its insets (TMM.10-fix2: a MIDI window's names column on the right)
    const float right = src.x + src.w - lb.x1;
    const Rect strip{sr.x0 + metrics::kLaneInset * k + dx, sr.y0 + metrics::kLaneTop * k + dy, sr.x1 - right * k + dx,
                     sr.y1 - metrics::kLaneBottom * k + dy};
    const float sw = strip.x1 - strip.x0;
    const Rect end{strip.x0 + c.u0 * sw, strip.y0, std::max(strip.x0 + c.u1 * sw, strip.x0 + c.u0 * sw + 1.0f), strip.y1};
    const Rect r = lerpRect(c.lane, end, e);
    dl.addRectFilled(r, ui::hex(c.colour));
  }
  dl.popClip();
  (void)p;
  stats_.lastMorphPaintMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---- the model ------------------------------------------------------------------------------------------------------

void ProjectPatchView::sync() {
  if (!stale_ && built_ == ctx_.doc()) return;
  rebuild();
}

void ProjectPatchView::rebuild() {
  const auto t0 = std::chrono::steady_clock::now();
  stale_ = false;
  const doc::Snapshot snap = ctx_.doc();
  const doc::DeviceLibrary* devlib = ctx_.deviceLibrary();
  doc::LowerOptions lo;
  lo.library = devlib != nullptr ? &devlib->library() : &emptyLibrary();
  lo.devices = devlib;  // user devices (T4.4-fix1) as the link resolves them
  if (!lowered_ || built_ != snap || loweredWith_ != devlib) {  // a nudge or an opened device: the same graph
    auto low = doc::lower(*snap, lo, lowered_.get());
    if (!low) {
      ctx_.setStatus("Project patch: " + low.error().message);
      lowered_ = std::make_shared<doc::LoweredProject>();
    } else {
      lowered_ = std::make_shared<doc::LoweredProject>(std::move(*low));
    }
    loweredWith_ = devlib;
  }
  built_ = snap;
  songEnd_ = 8 * kTicksPerWhole;
  doc::forEachTrack(*snap, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::ClipSlot& c : t.clips) songEnd_ = std::max(songEnd_, c.end());
  });
  // Expanded devices: their patches' own scenes (cached per patch name).
  std::vector<ExpandedSize> sizes;
  std::vector<InnerScene> inner;
  std::erase_if(expanded_, [&](doc::DeviceId d) { return doc::findDevice(*snap, d).device == nullptr; });
  std::erase_if(bandExpanded_, [&](doc::DeviceId d) { return doc::findDevice(*snap, d).device == nullptr; });
  if (openBand_.valid() && doc::findTrackPtr(*snap, openBand_) == nullptr) openBand_ = {};
  if (focus_.valid() && doc::findDevice(*snap, focus_).device == nullptr) focus_ = {};
  rackCircuits_.clear();
  for (const doc::DeviceId d : expanded_) {
    const doc::DeviceLoc loc = doc::findDevice(*snap, d);
    if (loc.device->rack) {  // T5.x-circuit-chain: a rack opens to its chains (nested frames), its devices to atoms
      circuit::ChainOptions co;
      co.devices = devlib;
      co.cache = &chainCache_;
      auto cc = std::make_shared<const circuit::ChainCircuit>(circuit::buildRackCircuit(*snap, d, co));
      if (cc->boxes.empty()) continue;
      const circuit::CircuitScene& rs = *cc->scene;
      sizes.push_back({d, rs.x1 - rs.x0, rs.y1 - rs.y0});
      inner.push_back({d, std::shared_ptr<const circuit::CircuitScene>(cc, cc->scene.get())});
      rackCircuits_.emplace_back(d, std::move(cc));
      continue;
    }
    // What the device plays: the stock patch, or a user device's resolved root (its name carries its graph hash, so
    // an edited text gets its own cache entry).
    std::shared_ptr<const doc::ResolvedDevice> hold;
    const graph::Patch* callee = devlib != nullptr ? devlib->patchOf(*loc.device, hold) : nullptr;
    if (callee == nullptr) continue;
    // T4.15 / TMM.12: as it plays (what its settings switch off hidden), cached per patch and settings.
    const std::string key =
        circuit::deviceSceneKey(hold && hold->graph ? hold->graph->root : loc.device->patch, loc.device->settings);
    std::shared_ptr<const circuit::CircuitScene> sc;
    for (const auto& [name, s] : innerCache_)
      if (name == key) sc = s;
    if (!sc) {
      if (innerCache_.size() >= kInnerCacheMax) innerCache_.erase(innerCache_.begin());  // oldest first
      sc = circuit::buildDeviceScene(*callee, loc.device->settings);
      innerCache_.emplace_back(key, sc);
    }
    sizes.push_back({d, sc->x1 - sc->x0, sc->y1 - sc->y0});
    inner.push_back({d, sc});
  }
  LayoutOptions opt;
  opt.sourceW = laneWidth();
  opt.expanded = sizes;
  opt.expandedMixers = expandedMixers_;
  opt.nudges = &ctx_.store().view().patchNudges;
  std::erase_if(closedGroups_, [&](doc::DeviceId d) { return doc::findDevice(*snap, d).device == nullptr; });
  // GU.5b: groups seen for the first time start closed when they have a panel (seenGroups_).
  doc::forEachTrack(*snap, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::DevicePtr& d : t.devices) {
      if (!doc::isGroupDevice(*d)) continue;
      const auto at = std::lower_bound(seenGroups_.begin(), seenGroups_.end(), d->id.value);
      if (at != seenGroups_.end() && *at == d->id.value) continue;
      seenGroups_.insert(at, d->id.value);
      if (d->hasPanel() && groupOpen(d->id)) closedGroups_.push_back(d->id);
    }
  });
  opt.closedGroups = closedGroups_;  // TX.groups
  opt.scope = scope_;
  if (freeRack_) opt.freeTrack = rackTrack_.valid() ? rackTrack_ : doc::TrackId{0xFFFFFFFFu};  // TX.free-rack (none: empty)
  if (selectedGroup_.valid() && doc::findDevice(*snap, selectedGroup_).device == nullptr) selectedGroup_ = {};
  if (panelsShown_) opt.panelSize = [this](const doc::Device& d) { return panelSize(d); };  // TX.panel-faces
  // TX.mod-all: wires into a face's or panel's params end on their controls (no made-up pins).
  if (canvas_->facesShown())
    opt.controlAnchor = [this](const doc::Device& d, std::string_view param) { return controlAnchorOf(d, param); };
  // Keep the selection (devices, wires) across the rebuild.
  std::vector<doc::DeviceId> selDevices = selectedDevices();
  std::vector<doc::WireId> selWires;
  if (canvas_->scene())
    for (const uint32_t w : canvas_->selection().wires)
      if (w < scene_.modelEdge.size() && scene_.modelEdge[w] != kNone && scene_.modelEdge[w] < patch_.edges.size()) {
        const Edge& e = patch_.edges[scene_.modelEdge[w]];
        if (e.kind == EdgeKind::Modulation) selWires.push_back(e.wire);
      }
  canvas_->beforeRebuild();  // TX.node-menu-fix1: a gesture's model indices, by identity
  patch_ = buildProjectPatch(*snap, *lowered_, devlib != nullptr ? &devlib->catalog() : nullptr, opt);
  tracksOf_ = nullptr;  // bandTrack's table follows the new bands
  scene_ = buildProjectScene(patch_, inner);
  const circuit::CircuitCanvas::View keep = canvas_->view();
  canvas_->setScene(scene_.scene, !fitted_ && !embedded(), true);
  if (!fitted_ && embedded()) fitEmbedded();  // TX.groups / TX.free-rack: a panel shows all of its contents
  if (fitted_) canvas_->setView(keep);
  fitted_ = true;
  syncPanels();  // TX.panel-faces: kept panels show the new document and their blocks' sizes
  std::vector<uint32_t> nodes, wires;
  for (const doc::DeviceId d : selDevices) {
    const uint32_t n = patch_.nodeOf(d);
    if (n != kNone && scene_.sceneNode[n] != kNone) nodes.push_back(scene_.sceneNode[n]);
  }
  for (uint32_t w = 0; w < scene_.modelEdge.size(); ++w)
    if (scene_.modelEdge[w] != kNone) {
      const Edge& e = patch_.edges[scene_.modelEdge[w]];
      if (e.kind == EdgeKind::Modulation && std::find(selWires.begin(), selWires.end(), e.wire) != selWires.end()) wires.push_back(w);
    }
  if (!nodes.empty() || !wires.empty()) canvas_->select(nodes, wires);
  canvas_->afterRebuild();
  readout_ = {};
  if (showCosts_) {
    costs_.clear();
    if (!costTimer_) pollCosts();
  }
  syncWindows();  // GU.5: the panel windows of the view state (a group gone: its window goes)
  ++stats_.rebuilds;
  stats_.lastRebuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  // TX.block-drag: blocks never overlap — every re-layout settles (not inside a push's own re-layout, nor while a block
  // is dragged or resized: their release does). Last: a push's immediate re-layout may rebuild again from here.
  if (!settleHeld_ && !pushing() && !blockGestureRunning()) {
    if (!moved_.empty() || !resized_.empty()) cascade_ = 0;
    if (cascade_ < kPushCascade) {
      ++cascade_;
      if (!startPush()) cascade_ = 0;  // nothing overlaps: the next change starts afresh
    }
    moved_.clear();
    resized_.clear();
  }
}

void ProjectPatchView::onDocument(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::Reset) {
    expanded_.clear();
    expandedMixers_.clear();
    innerCache_.clear();
    chainCache_.clear();
    bandExpanded_.clear();
    openBand_ = {};
    fitted_ = false;
    stale_ = true;
    if (shown()) sync();
    return;
  }
  const doc::ChangeSet& c = e.changes;
  bool rebuildNeeded = c.structural() || c.has(doc::kChangeMeta);
  if (!rebuildNeeded && c.has(doc::kChangeParam)) {
    // A send turned on or off changes its wire's look (dim at −∞): a rebuild; any other value only repaints.
    for (const Edge& ed : patch_.edges) {
      if (ed.kind != EdgeKind::Send) continue;
      const Band& to = patch_.bands[patch_.nodes[ed.to].band];
      float level = doc::kMinVolumeDb;
      if (const doc::Track* t = bandTrack(patch_.nodes[ed.from].band))
        for (const doc::Send& s : t->mixer.sends)
          if (s.target == to.track) level = s.levelDb;
      if ((!(level > doc::kMinVolumeDb)) != ed.silent) {
        rebuildNeeded = true;
        break;
      }
    }
  }
  if (rebuildNeeded) stale_ = true;
  if (!shown()) return;
  if (stale_) {
    rebuild();
  } else {
    ++stats_.repaints;
    built_ = ctx_.doc();  // same graph; the overlay reads the values
    syncPanels();         // TX.panel-faces: a param value shown on a block's panel
    canvas_->markDirty();
  }
  for (panel::PanelWindow* w : windows_)  // GU.5: the panels follow the document (values, elements)
    if (w->visible()) w->documentChanged();
}

const doc::Track* ProjectPatchView::bandTrack(uint32_t b) const noexcept {
  const doc::Snapshot& snap = ctx_.doc();
  if (tracksOf_ != snap || bandTracks_.size() != patch_.bands.size()) {
    tracksOf_ = snap;
    std::unordered_map<uint32_t, const doc::Track*> byId;
    doc::forEachTrack(*snap, [&](const doc::Track& t, const doc::Track*) { byId.emplace(t.id.value, &t); });
    bandTracks_.assign(patch_.bands.size(), nullptr);
    bandTunings_.assign(patch_.bands.size(), nullptr);
    for (size_t i = 0; i < patch_.bands.size(); ++i) {
      if (const auto it = byId.find(patch_.bands[i].track.value); it != byId.end()) bandTracks_[i] = it->second;
      if (hasLaneWindow(patch_.bands[i]) && bandTracks_[i] != nullptr)
        if (const std::optional<doc::TrackPath> path = doc::findTrack(*snap, patch_.bands[i].track)) bandTunings_[i] = doc::effectiveTuning(*snap, *path);
    }
  }
  return b < bandTracks_.size() ? bandTracks_[b] : nullptr;
}

const doc::TuningDef* ProjectPatchView::bandTuning(uint32_t b) const noexcept {
  (void)bandTrack(b);  // the table of this snapshot
  return b < bandTunings_.size() ? bandTunings_[b] : nullptr;
}

ArrangementView* ProjectPatchView::arrangement() const noexcept { return arrangementNear(*this); }

// ---- semantic zoom --------------------------------------------------------------------------------------------------

void ProjectPatchView::animateViewTo(const circuit::CircuitCanvas::View& v) {
  viewAnim_.active = true;
  viewAnim_.start = -1.0;
  viewAnim_.from = canvas_->view();
  viewAnim_.to = v;
  requestFrame();
}

namespace {
circuit::CircuitCanvas::View fitRect(const ui::IRect& r, float us, float x0, float y0, float x1, float y1, double maxZoom) {
  circuit::CircuitCanvas::View v;
  const double margin = 24.0;
  const double lw = r.w / static_cast<double>(us), lh = r.h / static_cast<double>(us);
  double z = std::min(lw / std::max(1.0, static_cast<double>(x1 - x0) + 2 * margin), lh / std::max(1.0, static_cast<double>(y1 - y0) + 2 * margin));
  z = std::clamp(z, circuit::CircuitCanvas::kMinZoom, maxZoom);
  v.zoom = z;
  v.x = static_cast<double>(x0 + x1) * 0.5 - lw / z * 0.5;
  v.y = static_cast<double>(y0 + y1) * 0.5 - lh / z * 0.5;
  return v;
}
}  // namespace

void ProjectPatchView::fitAll(bool animate) {
  if (embedded()) {  // TX.groups / TX.free-rack: an embedded view fits its own contents (and refits with its size)
    sync();
    if (canvas_->scene()) fitEmbedded();
    return;
  }
  closeBand();  // T5.x-circuit-chain: the whole project again (an opened band's devices close)
  sync();
  if (!canvas_->scene()) return;
  const circuit::CircuitScene& s = *canvas_->scene();
  const auto v = fitRect(canvas_->rect(), tree() ? tree()->scale() : 1.0f, s.x0, s.y0, s.x1, s.y1, 2.0);
  if (animate) animateViewTo(v);
  else canvas_->setView(v);
}

void ProjectPatchView::focusBand(doc::TrackId track, bool animate) {
  sync();
  const uint32_t b = patch_.bandOf(track);
  if (b == kNone) return;
  const Band& band = patch_.bands[b];
  const float x0 = band.kind == doc::TrackKind::Group ? band.cx0 : band.x0;
  const float x1 = band.kind == doc::TrackKind::Group ? band.cx1 : band.x1;
  const auto v = fitRect(canvas_->rect(), tree() ? tree()->scale() : 1.0f, x0, band.y, x1, band.y + band.h, 1.5);
  if (animate) animateViewTo(v);
  else canvas_->setView(v);
}

void ProjectPatchView::openBand(doc::TrackId track, bool animate) {
  sync();
  if (doc::findTrackPtr(*ctx_.doc(), track) == nullptr) return;
  expandBand(track);
  focusBand(track, animate);
}

void ProjectPatchView::expandBand(doc::TrackId track) {
  const doc::Track* t = doc::findTrackPtr(*ctx_.doc(), track);
  if (t == nullptr) return;
  if (openBand_ != track) closeBand();
  openBand_ = track;
  bool opened = false;
  for (const doc::DevicePtr& d : t->devices)
    if (!deviceExpanded(d->id)) {
      expanded_.push_back(d->id);
      bandExpanded_.push_back(d->id);
      opened = true;
    }
  if (opened) stale_ = true;
  sync();
}

void ProjectPatchView::closeBand() {
  focus_ = {};
  if (!openBand_.valid() && bandExpanded_.empty()) return;
  openBand_ = {};
  if (bandExpanded_.empty()) return;
  std::erase_if(expanded_, [&](doc::DeviceId d) { return std::find(bandExpanded_.begin(), bandExpanded_.end(), d) != bandExpanded_.end(); });
  bandExpanded_.clear();
  stale_ = true;
  if (shown()) sync();
}

const circuit::ChainCircuit* ProjectPatchView::rackCircuit(doc::DeviceId rack) const noexcept {
  for (const auto& [d, c] : rackCircuits_)
    if (d == rack) return c.get();
  return nullptr;
}

void ProjectPatchView::setDeviceExpanded(doc::DeviceId device, bool on) {
  const auto it = std::find(expanded_.begin(), expanded_.end(), device);
  if (on == (it != expanded_.end())) return;
  if (on) expanded_.push_back(device);
  else expanded_.erase(it);
  if (!on) std::erase(bandExpanded_, device);
  stale_ = true;
  sync();
  // Zoomed onto the opened device (semantic zoom: band → atoms); closing goes back to its band.
  const uint32_t n = patch_.nodeOf(device);
  if (n == kNone) return;
  if (on) {
    const Node& nd = patch_.nodes[n];
    animateViewTo(fitRect(canvas_->rect(), tree() ? tree()->scale() : 1.0f, nd.x, nd.y, nd.x + nd.w, nd.y + nd.h, 2.0));
  } else {
    focusBand(patch_.bands[patch_.nodes[n].band].track);
  }
}

bool ProjectPatchView::deviceExpanded(doc::DeviceId device) const noexcept {
  return std::find(expanded_.begin(), expanded_.end(), device) != expanded_.end();
}

void ProjectPatchView::setMixerExpanded(doc::TrackId track, bool on) {
  const auto it = std::find(expandedMixers_.begin(), expandedMixers_.end(), track);
  if (on == (it != expandedMixers_.end())) return;
  if (on) expandedMixers_.push_back(track);
  else expandedMixers_.erase(it);
  stale_ = true;
  sync();
}

bool ProjectPatchView::mixerExpanded(doc::TrackId track) const noexcept {
  return std::find(expandedMixers_.begin(), expandedMixers_.end(), track) != expandedMixers_.end();
}

// ---- the whole project as one circuit (T5.x-one-circuit) ----------------------------------------------------------

bool ProjectPatchView::deviceWorldRect(doc::DeviceId device, LaneRect& out) const {
  const uint32_t n = patch_.nodeOf(device);
  if (n == kNone) return false;
  const Node& nd = patch_.nodes[n];
  out = {nd.x, nd.y, nd.x + nd.w, nd.y + nd.h};
  if (nd.rack && nd.device != device && nd.expanded)  // inside an opened rack: its box in the rack's frame
    if (const circuit::ChainCircuit* cc = rackCircuit(nd.device)) {
      const uint32_t b = cc->boxOf(device);
      if (b != circuit::kNoBox) {
        const float dx = nd.x + metrics::kExpandedPad - cc->scene->x0, dy = nd.y + 16.0f + metrics::kExpandedPad - cc->scene->y0;
        const circuit::ChainBox& box = cc->boxes[b];
        out = {box.x0 + dx, box.y0 + dy, box.x1 + dx, box.y1 + dy};
      }
    }
  return true;
}

void ProjectPatchView::frameDevice(doc::DeviceId device, bool animate) {
  LaneRect r;
  if (!deviceWorldRect(device, r)) return;
  const auto v = fitRect(canvas_->rect(), tree() ? tree()->scale() : 1.0f, r.x0, r.y0, r.x1, r.y1, 1.5);
  if (animate) animateViewTo(v);
  else canvas_->setView(v);
}

bool ProjectPatchView::openCircuit(doc::TrackId track, doc::DeviceId device, bool animate) {
  const doc::Project& p = *ctx_.doc();
  if (device.valid()) {
    const doc::DeviceLoc loc = doc::findDevice(p, device);
    if (loc.device == nullptr || loc.track == nullptr) {
      ctx_.setStatus("No such device");
      return false;
    }
    if (!track.valid()) track = loc.track->id;
    if (loc.track->id != track) {
      ctx_.setStatus("The device is not on that track");
      return false;
    }
  }
  if (doc::findTrackPtr(p, track) == nullptr) {
    ctx_.setStatus("No track to show in the circuit view (select one first)");
    return false;
  }
  if (!shown() || morph_.active) {  // from another view: here at once (the morph would race the framing)
    setShown(true, false);
    animate = false;
    if (tree()) tree()->layoutNow();
  }
  sync();
  viewAnim_.active = false;
  expandBand(track);
  focus_ = device.valid() ? device : doc::DeviceId{};
  if (focus_.valid()) frameDevice(focus_, animate);
  else focusBand(track, animate);
  canvas_->markDirty();
  if (tree()) tree()->setFocus(canvas_);
  ctx_.setStatus("Double-click inside a device to edit its circuit; Ctrl+wheel out (or F) for the whole project");
  return true;
}

// ---- lane windows (T5.x-one-circuit) --------------------------------------------------------------------------------

float ProjectPatchView::laneWidth() const noexcept {
  if (laneUnit_ <= 0 || laneSpan_ <= laneUnit_) return metrics::kSourceW;
  const double w = static_cast<double>(metrics::kSourceW) * static_cast<double>(laneSpan_) / static_cast<double>(laneUnit_);
  return std::min(metrics::kSourceMaxW, std::ceil(static_cast<float>(w) / 4.0f) * 4.0f);
}

void ProjectPatchView::initLaneTime() {
  const doc::Project& p = *ctx_.doc();
  const TimeSigMap& sig = p.timeline->sigMap;
  Ticks at = 0;
  if (const PlayheadSnapshot ph = ctx_.transport().playhead(); ph.playing) at = ph.tick;
  else if (const ArrangementView* arr = arrangementNear(*this)) at = static_cast<Ticks>(std::max(0.0, arr->originTick()));
  const int64_t bar = sig.ticksToBarBeat(std::max<Ticks>(0, at)).bar;
  const Ticks start = sig.barToTicks(bar);
  laneUnit_ = sig.barToTicks(bar + kLaneBars) - start;  // 4 bars of the signature there (never literal ticks)
  if (!laneInit_) {
    laneSpan_ = laneUnit_;
    laneInit_ = true;
  }
  setLaneTime(start, laneSpan_);
}

void ProjectPatchView::setLaneTime(Ticks start, Ticks span) {
  const doc::Project& p = *ctx_.doc();
  const TimeSigMap& sig = p.timeline->sigMap;
  start = std::max<Ticks>(0, start);
  if (laneUnit_ <= 0) {
    const int64_t bar = sig.ticksToBarBeat(start).bar;
    laneUnit_ = sig.barToTicks(bar + kLaneBars) - sig.barToTicks(bar);
    laneInit_ = true;
  }
  const Ticks beat = ticksPerBeat(sig.signatureAt(start));
  span = std::clamp(span, beat, std::max(beat, laneUnit_ * (kLaneMaxBars / kLaneBars)));
  if (start == laneStart_ && span == laneSpan_) return;
  const float w0 = laneWidth();
  laneStart_ = start;
  laneSpan_ = span;
  if (laneWidth() != w0) relayout();  // the windows widen / narrow: the bands re-lay out (the chains move)
  else damageLanes();                 // the same width: only the windows repaint
}

void ProjectPatchView::scrollLanes(double notches) {
  setLaneTime(laneStart_ + static_cast<Ticks>(std::llround(notches * kLaneScrollStep * static_cast<double>(laneSpan_))), laneSpan_);
}

void ProjectPatchView::zoomLanes(double notches, float anchor) {
  const double u = std::clamp(static_cast<double>(anchor), 0.0, 1.0);
  const double at = static_cast<double>(laneStart_) + u * static_cast<double>(laneSpan_);
  const auto span = static_cast<Ticks>(std::llround(static_cast<double>(laneSpan_) / std::pow(kLaneZoomStep, notches)));
  setLaneTime(static_cast<Ticks>(std::llround(at - u * static_cast<double>(span))), span);
}

void ProjectPatchView::relayout() {
  stale_ = true;
  if (!shown()) return;
  const auto t0 = std::chrono::steady_clock::now();
  rebuild();
  ++stats_.relayouts;
  stats_.lastRelayoutMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<ProjectPatchView::LaneClip> ProjectPatchView::laneClips(uint32_t band) const {
  std::vector<LaneClip> out;
  if (band >= patch_.bands.size() || !hasLaneWindow(patch_.bands[band]) || patch_.bands[band].source == kNone || laneSpan_ <= 0) return out;
  const doc::Track* t = bandTrack(band);
  if (t == nullptr) return out;
  const LaneRect r = laneBody(patch_.nodes[patch_.bands[band].source]);
  const double k = static_cast<double>(r.x1 - r.x0) / static_cast<double>(laneSpan_);
  const Ticks end = laneStart_ + laneSpan_;
  for (const doc::ClipSlot& c : t->clips) {
    if (c.end() <= laneStart_ || c.start() >= end) continue;
    LaneClip lc;
    lc.clip = c.id;
    lc.audio = c.isAudio();
    lc.x0 = r.x0 + static_cast<float>(static_cast<double>(std::max(c.start(), laneStart_) - laneStart_) * k);
    lc.x1 = r.x0 + static_cast<float>(static_cast<double>(std::min(c.end(), end) - laneStart_) * k);
    out.push_back(lc);
  }
  return out;
}

uint32_t ProjectPatchView::laneAt(Vec2 device) const {
  const graph::Point2 w = canvas_->toWorld(device);
  for (uint32_t i = 0; i < patch_.bands.size(); ++i) {
    const Band& b = patch_.bands[i];
    if (!hasLaneWindow(b) || b.source == kNone) continue;
    const Node& n = patch_.nodes[b.source];
    if (w.x >= n.x && w.x < n.x + n.w && w.y >= n.y && w.y < n.y + n.h) return i;
  }
  return kNone;
}

doc::ClipRef ProjectPatchView::laneClipAt(Vec2 device) const {
  const uint32_t b = laneAt(device);
  if (b == kNone || laneSpan_ <= 0) return {};
  const doc::Track* t = bandTrack(b);
  if (t == nullptr) return {};
  const LaneRect r = laneBody(patch_.nodes[patch_.bands[b].source]);
  const graph::Point2 w = canvas_->toWorld(device);
  if (w.x < r.x0 || w.x >= r.x1 || w.y < r.y0 || w.y >= r.y1) return {};
  const auto tick = static_cast<Ticks>(std::floor(static_cast<double>(laneStart_) +
                                                  static_cast<double>(w.x - r.x0) / static_cast<double>(r.x1 - r.x0) * static_cast<double>(laneSpan_)));
  doc::ClipRef hit;
  for (const doc::ClipSlot& c : t->clips)  // the last one starting at or before the tick wins (drawn on top)
    if (c.start() <= tick && tick < c.end()) hit = {t->id, c.id};
  return hit;
}

void ProjectPatchView::openClip(const doc::ClipRef& clip) {
  if (!clip.clip.valid()) return;
  ctx_.setSelectedTrack(clip.track);
  if (ArrangementView* arr = arrangementNear(*this)) arr->selectClips(std::span<const doc::ClipRef>(&clip, 1), true);
  ctx_.setDetailClip(clip);
  ctx_.setDetailVisible(true);
  ctx_.setDetailMode(DetailMode::Clip);
  if (onOpenClip) onOpenClip(clip);
}

void ProjectPatchView::damageLanes() {
  ui::UiTree* tr = tree();
  if (tr == nullptr || !shown()) return;
  const ui::IRect cr = canvas_->rect();
  for (const Band& b : patch_.bands) {
    if (!hasLaneWindow(b) || b.source == kNone) continue;
    const Node& n = patch_.nodes[b.source];
    const Rect r = canvas_->deviceRect(n.x, n.y, n.w, n.h);
    const ui::IRect d = ui::intersect(toIRect({r.x0 - 1.0f, r.y0 - 1.0f, r.x1 + 1.0f, r.y1 + 1.0f}), cr);
    if (!d.empty()) tr->damage().add(d.toRect());
  }
}

void ProjectPatchView::damageLaneColumn(Ticks t) {
  ui::UiTree* tr = tree();
  if (tr == nullptr || !shown() || laneSpan_ <= 0 || t < laneStart_ || t >= laneStart_ + laneSpan_) return;
  const ui::IRect cr = canvas_->rect();
  const float w = static_cast<float>(std::max(1, tr->uiScale().px(1.0f)));
  for (const Band& b : patch_.bands) {
    if (!hasLaneWindow(b) || b.source == kNone) continue;
    const Rect body = laneBodyDevice(*canvas_, patch_.nodes[b.source]);
    if (body.y1 < static_cast<float>(cr.y) || body.y0 > static_cast<float>(cr.bottom()) || body.x1 < static_cast<float>(cr.x) ||
        body.x0 > static_cast<float>(cr.right()))
      continue;
    const float x = lanePlayheadX(body, t, laneStart_, laneSpan_);
    const ui::IRect col = ui::intersect(toIRect({x - 1.0f, body.y0, x + w + 1.0f, body.y1}), cr);
    if (!col.empty()) tr->damage().add(col.toRect());
  }
}

// Called on transport notifications (play / stop, the start marker) and when shown or hidden: the only reads while
// stopped; nothing at all while hidden.
void ProjectPatchView::refreshTransport() {
  const bool on = shown();
  PlayheadSnapshot ph{};
  if (on) {
    ++stats_.playheadReads;
    ph = ctx_.transport().playhead();
  }
  if (on && ph.playing) {
    wakeFaces();  // TMM.5: voices start with the transport (cheap while a face timer already runs: nothing)
    canvas_->wakeCables();  // TX.cables: the glow polls while the sources are live (nothing while it already does)
    if (!playhead_ || *playhead_ != ph.tick) {
      if (playhead_) damageLaneColumn(*playhead_);
      playhead_ = ph.tick;
      damageLaneColumn(ph.tick);
    }
    if (!playheadTimer_) schedulePlayhead();
    return;
  }
  if (playhead_) {
    damageLaneColumn(*playhead_);
    playhead_.reset();
  }
  if (playheadTimer_) {
    cancelTimer(playheadTimer_);
    playheadTimer_ = 0;
  }
}

// While playing, wake when the playhead will have moved about one device px in the windows (from the tempo there),
// bounded to one frame at 144 Hz … 100 ms.
void ProjectPatchView::schedulePlayhead() {
  const doc::Project& p = *ctx_.doc();
  const double bpm = p.timeline->tempoMap.bpmAt(playhead_.value_or(laneStart_));
  const double ticksPerSecond = bpm / 60.0 * static_cast<double>(kPpq);
  const double pxPerTick = static_cast<double>(laneWidth() * canvas_->deviceScale()) / static_cast<double>(std::max<Ticks>(laneSpan_, 1));
  const double secondsPerPx = 1.0 / std::max(1e-12, pxPerTick * ticksPerSecond);
  playheadTimer_ = requestTimer(std::clamp(secondsPerPx, 1.0 / 144.0, 0.1));
}

void ProjectPatchView::readPlayhead() {
  if (!shown()) {
    refreshTransport();
    return;
  }
  ++stats_.playheadReads;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  if (!ph.playing) {
    refreshTransport();
    return;
  }
  const Ticks end = laneStart_ + laneSpan_;
  const bool wasIn = playhead_ && *playhead_ >= laneStart_ && *playhead_ < end;
  if (wasIn && (ph.tick >= end || ph.tick < laneStart_)) {
    // Follow: the windows page on (by whole spans; back to the playhead's bar after a jump back, a loop).
    playhead_ = ph.tick;
    Ticks start = laneStart_;
    if (ph.tick >= end) start = laneStart_ + laneSpan_ * ((ph.tick - laneStart_) / laneSpan_);
    else start = ctx_.doc()->timeline->sigMap.barToTicks(ctx_.doc()->timeline->sigMap.ticksToBarBeat(ph.tick).bar);
    setLaneTime(start, laneSpan_);  // repaints the windows (or re-lays out)
    schedulePlayhead();
    return;
  }
  if (!playhead_ || *playhead_ != ph.tick) {
    const double k = static_cast<double>(laneWidth() * canvas_->deviceScale()) / static_cast<double>(std::max<Ticks>(laneSpan_, 1));
    const bool moved = !playhead_ || std::floor(static_cast<double>(*playhead_ - laneStart_) * k) != std::floor(static_cast<double>(ph.tick - laneStart_) * k);
    if (moved && playhead_) damageLaneColumn(*playhead_);
    playhead_ = ph.tick;
    if (moved) damageLaneColumn(ph.tick);
  }
  schedulePlayhead();
}

// ---- edits ----------------------------------------------------------------------------------------------------------

bool ProjectPatchView::apply(const doc::ops::EditResult& e, const char* what) {
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return true;
}

bool ProjectPatchView::dispatch(const doc::Command& c, const char* what) {
  // TMM.13-fix2 (H1): nothing of `this` is read after the store call (its listeners may have replaced this view's
  // host; DeviceChainView now retires frames until the next frame, and this keeps the function safe regardless).
  ViewContext& ctx = ctx_;
  const doc::Snapshot before = ctx.doc();
  if (Status s = ctx.store().dispatch(c); !s) {
    ctx.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return ctx.doc() != before;
}

bool ProjectPatchView::connectEnds(doc::Wire ends) {
  // TX.all-free: a cable between two main ports is written with kMainPort ends (a link of the main path when it lies
  // on it: doc::canonicalWire).
  const doc::Wire w = doc::canonicalWire(*ctx_.doc(), std::move(ends), ctx_.deviceLibrary());
  return apply(doc::ops::connect(*ctx_.doc(), ctx_.store().ids(), w), "Wire");
}

bool ProjectPatchView::connect(doc::DeviceId from, const std::string& port, doc::DeviceId to, const std::string& param) {
  doc::Wire w;
  w.from = from;
  w.port = port;
  w.to = to;
  w.param = param;
  return connectEnds(std::move(w));
}

bool ProjectPatchView::connectNotes(doc::TrackId track, const std::string& port, doc::DeviceId to, const std::string& param) {
  doc::Wire w;
  w.noteSource = track;
  w.port = port;
  w.to = to;
  w.param = param;
  return connectEnds(std::move(w));
}

bool ProjectPatchView::sendOn(doc::TrackId track, doc::TrackId ret) {
  return dispatch(doc::Batch{"Send On", {doc::SetSendLevel{track, ret, 0.0f}}}, "Send");
}

bool ProjectPatchView::routeTo(doc::TrackId track, doc::TrackId target) {
  const doc::Project& p = *ctx_.doc();
  const doc::Track* t = doc::findTrackPtr(p, target);
  const doc::Track* src = doc::findTrackPtr(p, track);
  if (t == nullptr || src == nullptr || track == target) return false;
  if (t->kind == doc::TrackKind::Return) return sendOn(track, target);
  doc::Output o;
  if (t->kind == doc::TrackKind::Master) {
    o.kind = doc::OutputKind::Master;
  } else if (t->kind == doc::TrackKind::Group) {
    o.kind = doc::OutputKind::Track;
    o.track = target;
    // Into the group the track is in: that is its default.
    if (const auto path = doc::findTrack(p, track))
      if (const doc::Track* parentGroup = doc::parentOf(p, *path); parentGroup != nullptr && parentGroup->id == target) o = {};
  } else {
    ctx_.setStatus("Route: a track's output goes to a group, a return (its send) or the master");
    return false;
  }
  if (src->mixer.output == o) return false;
  return dispatch(doc::Batch{"Route Output", {doc::SetTrackOutput{track, o}}}, "Route");
}

bool ProjectPatchView::addDevice(doc::TrackId track, const std::string& patch, uint32_t index) {
  if (patch.starts_with(doc::kMetaDevicePrefix))  // TMM.10: a chain meta device (its kind follows the slot)
    return apply(doc::ops::addMetaDevice(*ctx_.doc(), ctx_.store().ids(), track, patch.substr(doc::kMetaDevicePrefix.size()), index),
                 "Add Device");
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  const doc::DeviceInfo* info = lib != nullptr ? lib->catalog().find(patch) : nullptr;
  const doc::DeviceKind kind = info != nullptr ? info->kind : doc::DeviceKind::AudioEffect;
  return apply(doc::ops::addDevice(*ctx_.doc(), ctx_.store().ids(), track, patch, kind, index), "Add Device");
}

bool ProjectPatchView::addFreeDevice(doc::TrackId track, const std::string& patch) {
  // TX.all-free: a block placed unwired (doc::ops::kOffPath): it plays once a cable reaches it.
  const doc::Project& p = *ctx_.doc();
  if (patch.starts_with(doc::kMetaDevicePrefix))
    return apply(doc::ops::addMetaDevice(p, ctx_.store().ids(), track, patch.substr(doc::kMetaDevicePrefix.size()), doc::ops::kOffPath),
                 "Add Device");
  if (patch.starts_with(doc::kModuleItemPrefix)) {  // GU.4: a built-in module: its group preset, pasted
    const doc::ModuleInfo* m = doc::findModule(std::string_view(patch).substr(doc::kModuleItemPrefix.size()));
    if (m == nullptr) return false;
    return apply(doc::ops::pasteBlocks(p, ctx_.store().ids(), track, doc::moduleBlocks(m->kind)), "Add Module");
  }
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  const doc::DeviceInfo* info = lib != nullptr ? lib->catalog().find(patch) : nullptr;
  const doc::DeviceKind kind = info != nullptr ? info->kind : doc::DeviceKind::AudioEffect;
  return apply(doc::ops::addDevice(p, ctx_.store().ids(), track, patch, kind, doc::ops::kOffPath), "Add Device");
}

bool ProjectPatchView::takeOffPath(std::span<const doc::DeviceId> devices) {
  return apply(doc::ops::takeOffPath(*ctx_.doc(), ctx_.store().ids(), ctx_.deviceLibrary(), devices), "Take Off Path");
}

// ---- TX.free-rack ---------------------------------------------------------------------------------------------------

void ProjectPatchView::fitEmbedded() {
  canvas_->fitAll();
  fitRect_ = canvas_->rect();
  fitView_ = canvas_->view();
}

namespace {
std::optional<Vec2> clampedPoint(Vec2 p, const ui::IRect& r, bool* docked) {
  const float x = std::clamp(p.x, static_cast<float>(r.x), static_cast<float>(r.right()));
  const float y = std::clamp(p.y, static_cast<float>(r.y), static_cast<float>(r.bottom()));
  if (docked != nullptr) *docked = x == p.x && y == p.y;
  return Vec2{x, y};
}
}  // namespace

std::optional<Vec2> ProjectPatchView::outputPoint(doc::DeviceId device, std::string_view port, bool* docked) const {
  if (!canvas_->scene() || !shown()) return std::nullopt;
  const uint32_t n = patch_.nodeOf(device);
  if (n == kNone) return std::nullopt;
  const Node& nd = patch_.nodes[n];
  int32_t k = nd.output(port);
  if (k < 0) k = nd.output(std::string(port) + "@" + std::to_string(device.value));  // a closed group's member
  if (k < 0) k = 0;
  if (nd.outputs.empty()) return std::nullopt;
  return clampedPoint(canvas_->toDevice(canvas_->portWorld(n, static_cast<uint32_t>(k), false)), canvas_->rect(), docked);
}

std::optional<Vec2> ProjectPatchView::inputPoint(doc::DeviceId device, std::string_view port, bool* docked) const {
  if (!canvas_->scene() || !shown()) return std::nullopt;
  const uint32_t n = patch_.nodeOf(device);
  if (n == kNone) return std::nullopt;
  const Node& nd = patch_.nodes[n];
  int32_t k = nd.input(port);
  if (k < 0) k = nd.input(std::string(port) + "@" + std::to_string(device.value));
  graph::Point2 w{nd.x, nd.y + pinOffset(nd, 0)};
  if (k >= 0) w = canvas_->portWorld(n, static_cast<uint32_t>(k), true);  // TX.orbit-ports: an orbiting port where it is
  return clampedPoint(canvas_->toDevice(w), canvas_->rect(), docked);
}

ProjectPatchView::WireTarget ProjectPatchView::wireTargetAt(Vec2 pos) const {
  if (!canvas_->scene() || !shown() || !canvas_->rect().contains(pos)) return {};
  const ProjectCanvas::PinHit pin = canvas_->pinAt(pos);
  if (pin.valid() && pin.input && pin.node < patch_.nodes.size()) {
    const Node& t = patch_.nodes[pin.node];
    if ((t.kind == NodeKind::Device || (t.kind == NodeKind::Terminal && !t.inputs.empty())) && pin.port < t.inputs.size()) {
      const Port& q = t.inputs[pin.port];
      if (q.name != "notes" && !(t.rack && !q.param)) {
        std::string param = q.name;
        doc::DeviceId to = t.device;
        if (const size_t at = param.rfind('@'); at != std::string::npos && (t.rack || t.group)) {
          to = doc::DeviceId{static_cast<uint32_t>(std::stoul(param.substr(at + 1)))};
          param.resize(at);
        }
        return {to, param};
      }
    }
  }
  const circuit::CircuitCanvas::FaceParamHit fh = canvas_->faceParamAt(pos);
  if (!fh.param.empty() && fh.node < scene_.modelNode.size()) {
    const uint32_t mn = scene_.modelNode[fh.node];
    if (mn < patch_.nodes.size() && patch_.nodes[mn].kind == NodeKind::Device) {
      if (patch_.nodes[mn].groupPanel) {  // GU.5: a closed group's panel element: the control inside the group
        const doc::Binding b = panel::GroupPanelFace::controlOfKey(patch_.nodes[mn].device, fh.param);
        if (b.bound()) return {b.device, b.name};
      }
      return {patch_.nodes[mn].device, fh.param};
    }
  }
  return {};
}

std::vector<ui::MenuItem> ProjectPatchView::addFreeDeviceItems(doc::TrackId track, std::function<void(const std::string&)> pick) {
  if (!pick) pick = [this, track](const std::string& patch) { (void)addFreeDevice(track, patch); };
  std::vector<ui::MenuItem> addFree;
  const doc::Track* host = doc::findTrackPtr(*ctx_.doc(), track);
  const bool midi = host != nullptr && host->kind == doc::TrackKind::Midi;
  if (const doc::DeviceLibrary* lib = ctx_.deviceLibrary())
    for (const doc::DeviceInfo& d : lib->catalog().devices()) {
      // TX.all-free-fix1: a stock instrument is a block too (it plays the notes a cable gives it): on MIDI tracks.
      if (d.kind == doc::DeviceKind::Instrument && !midi) continue;
      const std::string patch = d.patch;
      addFree.push_back(ui::MenuItem::item(d.name, [pick, patch] { pick(patch); }));
    }
  if (!addFree.empty()) {
    addFree.push_back(ui::MenuItem::sep());
    ui::MenuItem head = ui::MenuItem::item("Meta Modules");
    head.enabled = false;
    addFree.push_back(std::move(head));
    for (const meta::Type* t : metaModuleTypes(true)) {
      const std::string patch = std::string(doc::kMetaDevicePrefix) + std::string(t->id);
      addFree.push_back(ui::MenuItem::item("   " + std::string(t->name), [pick, patch] { pick(patch); }));
    }
    for (const doc::ModuleInfo& m : doc::moduleInfos()) {  // GU.4: the modules (group presets)
      const std::string patch = std::string(doc::kModuleItemPrefix) + std::string(m.key);
      addFree.push_back(ui::MenuItem::item("   " + std::string(m.name), [pick, patch] { pick(patch); }));
    }
  }
  return addFree;
}

ui::Menu* ProjectPatchView::openRackMenu(Vec2 at) {
  std::vector<ui::MenuItem> items;
  ui::MenuItem add = ui::MenuItem::sub("Add Free Device", addFreeDeviceItems(rackTrack_));
  add.enabled = rackTrack_.valid() && !add.submenu.empty();
  items.push_back(std::move(add));
  items.push_back(ui::MenuItem::sep());
  items.push_back(ui::MenuItem::item("Zoom to Fit", [this] { fitEmbedded(); }, "F"));
  return ui::openContextMenu(*canvas_, at, std::move(items));
}

void ProjectPatchView::rebindFaces() { canvas_->rebindFaces(); }

void ProjectPatchView::wakeFaces() {
  if (shown() && !canvas_->animatingFaces()) canvas_->wakeFaces();
  if (shown()) canvas_->wakeCables();  // TX.cables: a source may have become live
}

// ---- TX.cables: the glow ------------------------------------------------------------------------------------------

bool ProjectPatchView::cableSignalLive() const {
  const ModulationSource* mod = ctx_.modulationSource();
  return (meters_ != nullptr && meters_->live()) || (mod != nullptr && mod->modulationLive());
}

// A cable's level: a wire into a param, how far its modulation swept the param since the last poll; an audio or note
// path (chain, notes, a wire into an input, a group's terminals), its track's peak level (−48 dB … 0 dB → 0 … 1). One
// meter read and one modulation read per poll.
void ProjectPatchView::cableLevels(std::span<const uint32_t> wires, std::span<float> levels) {
  std::vector<doc::TrackId> tracks;
  std::vector<ModTarget> targets;
  std::vector<int32_t> slot(wires.size(), -1);  // ≥ 0: track index; ≤ −2: −2 − target index
  const doc::Project* p = ctx_.doc().get();
  ModulationSource* mod = ctx_.modulationSource();
  for (size_t i = 0; i < wires.size(); ++i) {
    const uint32_t w = wires[i];
    if (w >= scene_.modelEdge.size() || scene_.modelEdge[w] >= patch_.edges.size()) continue;
    const Edge& e = patch_.edges[scene_.modelEdge[w]];
    if (e.kind == EdgeKind::Modulation && mod != nullptr && p != nullptr && e.toPort < patch_.nodes[e.to].inputs.size() &&
        patch_.nodes[e.to].inputs[e.toPort].param) {
      if (const doc::Wire* dw = doc::findWire(*p, e.wire)) {
        slot[i] = -2 - static_cast<int32_t>(targets.size());
        targets.push_back({dw->to, dw->param});
        continue;
      }
    }
    if (meters_ == nullptr || e.from >= patch_.nodes.size() || patch_.nodes[e.from].band >= patch_.bands.size()) continue;
    const doc::TrackId t = patch_.bands[patch_.nodes[e.from].band].track;
    const auto it = std::find(tracks.begin(), tracks.end(), t);
    slot[i] = static_cast<int32_t>(it - tracks.begin());
    if (it == tracks.end()) tracks.push_back(t);
  }
  std::vector<ui::MeterLevels> meters(tracks.size());
  if (!tracks.empty()) meters_->read(tracks, meters);
  std::vector<ModSweep> sweeps(targets.size());
  if (!targets.empty()) mod->readModulation(targets, sweeps);
  for (size_t i = 0; i < wires.size(); ++i) {
    float level = 0.0f;
    if (slot[i] >= 0) {
      const ui::MeterLevels& m = meters[static_cast<size_t>(slot[i])];
      const float peak = std::max(m.peakDb[0], m.channels > 1 ? m.peakDb[1] : -200.0f);
      level = std::clamp((peak + 48.0f) / 48.0f, 0.0f, 1.0f);
    } else if (slot[i] <= -2) {
      const ModSweep& sw = sweeps[static_cast<size_t>(-2 - slot[i])];
      const float swept = std::max(sw.up, sw.down);
      level = sw.known && swept > 1e-4f ? std::clamp(0.35f + swept, 0.0f, 1.0f) : 0.0f;
    }
    levels[i] = level;
  }
}

bool ProjectPatchView::editDeviceCircuit(doc::DeviceId device) {
  return circuit::openDeviceInCircuitView(*canvas_, ctx_, device) != nullptr;
}

bool ProjectPatchView::moveDevice(doc::DeviceId device, doc::TrackId toTrack, uint32_t index) {
  return apply(doc::ops::moveDevice(*ctx_.doc(), device, toTrack, index), "Move Device");
}

bool ProjectPatchView::moveToTrack(doc::DeviceId device, doc::TrackId toTrack, graph::Point2 at) {
  // TX.all-free: a block dropped on another band moves to that track off its path (its old path joined where it was),
  // where it was dropped.
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc l = doc::findDevice(p, device);
  if (l.device == nullptr || l.chain != nullptr || l.track->id == toTrack) return false;
  doc::ops::EditResult off = doc::ops::takeOffPath(p, ctx_.store().ids(), ctx_.deviceLibrary(), std::span<const doc::DeviceId>(&device, 1));
  if (!off) return apply(off, "Move Device");
  doc::ChangeSet cs;
  auto mid = doc::apply(ctx_.doc(), off->command, cs);
  if (!mid) {
    ctx_.setStatus("Move Device: " + mid.error().message);
    return false;
  }
  doc::ops::EditResult move = doc::ops::moveDevice(**mid, device, toTrack, doc::kAppend);
  if (!move) return apply(move, "Move Device");
  if (!dispatch(doc::Batch{"Move Device", {off->command, move->command}}, "Move Device")) return false;
  placeAt(device, at);
  return true;
}

bool ProjectPatchView::moveToTrack(std::span<const doc::DeviceId> devices, doc::TrackId toTrack, std::span<const graph::Point2> at) {
  if (devices.size() == 1 && at.size() == 1) return moveToTrack(devices.front(), toTrack, at.front());
  // TX.block-drag: a selection dropped on another band — those of other tracks off their paths, then each to the track
  // (each step applied to the scratch snapshot the next one reads); one Batch, one undo step.
  std::vector<doc::DeviceId> moving;
  for (const doc::DeviceId d : devices) {
    const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), d);
    if (l.device != nullptr && l.chain == nullptr && l.track->id != toTrack) moving.push_back(d);
  }
  if (moving.empty()) return false;
  std::vector<doc::Command> steps;
  doc::Snapshot snap = ctx_.doc();
  doc::ops::EditResult off = doc::ops::takeOffPath(*snap, ctx_.store().ids(), ctx_.deviceLibrary(), moving);
  if (!off) return apply(off, "Move Devices");
  doc::ChangeSet cs;
  if (!off->empty()) {
    auto mid = doc::apply(snap, off->command, cs);
    if (!mid) {
      ctx_.setStatus("Move Devices: " + mid.error().message);
      return false;
    }
    snap = *mid;
    steps.push_back(off->command);
  }
  for (const doc::DeviceId d : moving) {
    doc::ops::EditResult move = doc::ops::moveDevice(*snap, d, toTrack, doc::kAppend);
    if (!move) return apply(move, "Move Devices");
    auto next = doc::apply(snap, move->command, cs);
    if (!next) {
      ctx_.setStatus("Move Devices: " + next.error().message);
      return false;
    }
    snap = *next;
    steps.push_back(move->command);
  }
  if (!dispatch(doc::Batch{"Move Devices", std::move(steps)}, "Move Devices")) return false;
  settleHeld_ = true;  // all placed, then the overlap rule once
  for (size_t i = 0; i < devices.size() && i < at.size(); ++i)
    if (std::find(moving.begin(), moving.end(), devices[i]) != moving.end()) placeAt(devices[i], at[i]);
  settleHeld_ = false;
  (void)settleOverlaps();
  return true;
}

bool ProjectPatchView::toggleDevicesActive(std::span<const doc::DeviceId> devices) {
  return apply(doc::ops::toggleDevicesActive(*ctx_.doc(), devices), "Device Activator");
}

std::vector<doc::DeviceId> ProjectPatchView::selectedDevices() const {
  std::vector<doc::DeviceId> out;
  if (selectedGroup_.valid()) out.push_back(selectedGroup_);  // TX.groups: an opened capsule (no scene node)
  if (!canvas_->scene()) return out;
  for (const uint32_t v : canvas_->selection().nodes) {
    if (v >= scene_.modelNode.size()) continue;
    const uint32_t n = scene_.modelNode[v];
    if (n >= patch_.nodes.size() || patch_.nodes[n].kind != NodeKind::Device) continue;
    if (std::find(out.begin(), out.end(), patch_.nodes[n].device) == out.end()) out.push_back(patch_.nodes[n].device);
  }
  return out;
}

bool ProjectPatchView::deleteSelection() {
  if (!canvas_->scene()) return false;
  const doc::Snapshot start = ctx_.doc();
  doc::Snapshot cur = start;
  doc::Batch batch{"Delete", {}};
  auto chain = [&](const doc::Command& c) {
    doc::ChangeSet cs;
    auto r = doc::apply(cur, c, cs);
    if (!r) {
      ctx_.setStatus("Delete: " + r.error().message);
      return false;
    }
    cur = *r;
    batch.commands.push_back(c);
    return true;
  };
  const std::vector<doc::DeviceId> devices = selectedDevices();
  if (!devices.empty()) {
    auto e = doc::ops::deleteDevices(*cur, devices);
    if (!e) {
      ctx_.setStatus("Delete: " + e.error().message);
      return false;
    }
    if (!e->empty() && !chain(e->command)) return false;
  }
  for (const uint32_t w : canvas_->selection().wires) {
    if (w >= scene_.modelEdge.size() || scene_.modelEdge[w] == kNone) continue;
    const Edge& e = patch_.edges[scene_.modelEdge[w]];
    const Band& from = patch_.bands[patch_.nodes[e.from].band];
    const Band& to = patch_.bands[patch_.nodes[e.to].band];
    switch (e.kind) {
      case EdgeKind::Modulation:
        if (doc::findWire(*cur, e.wire) != nullptr && !chain(doc::RemoveWire{e.wire})) return false;
        break;
      case EdgeKind::Send:
        if (!chain(doc::SetSendLevel{from.track, to.track, doc::kMinVolumeDb})) return false;
        break;
      case EdgeKind::Route:
        if (!chain(doc::SetTrackOutput{from.track, doc::Output{doc::OutputKind::None, {}}})) return false;
        break;
      case EdgeKind::Notes:
      case EdgeKind::Chain: break;  // implied by the chain: delete a device instead
    }
  }
  if (batch.commands.empty() || cur == start) return false;
  canvas_->clearSelection();
  return dispatch(batch, "Delete");
}

// ---- TX.groups ------------------------------------------------------------------------------------------------------

bool ProjectPatchView::groupDevices(std::span<const doc::DeviceId> devices) {
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (lib == nullptr) {
    ctx_.setStatus("Group: no device library");
    return false;
  }
  doc::ops::EditResult e = doc::ops::groupInCircuit(ctx_.doc(), ctx_.store().ids(), *lib, devices);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  const doc::DeviceId g = e->devices.empty() ? doc::DeviceId{} : e->devices.front();
  if (!apply(e, "Group")) return false;
  canvas_->clearSelection();
  selectGroup(g);
  ctx_.setStatus("Grouped: Ctrl+Shift+G ungroups");
  return true;
}

bool ProjectPatchView::ungroup(doc::DeviceId group) {
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (lib == nullptr) return false;
  doc::ops::EditResult e = doc::ops::ungroupInCircuit(ctx_.doc(), ctx_.store().ids(), *lib, group);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  if (selectedGroup_ == group) selectedGroup_ = {};
  return apply(e, "Ungroup");
}

bool ProjectPatchView::groupSelection() {
  std::vector<doc::DeviceId> d = selectedDevices();
  // TMM.13-fix2 (M5): a group's own panel never offers the group itself (its capsule frame selects it there).
  if (scope_.valid()) std::erase(d, scope_);
  if (d.empty()) {
    if (scope_.valid()) return false;  // unhandled: the Device view's Ctrl+G acts on its own selection
    ctx_.setStatus("Group: select the devices first (click, Ctrl+click or drag a box around them)");
    return false;
  }
  return groupDevices(d);
}

bool ProjectPatchView::ungroupSelection() {
  std::vector<doc::DeviceId> groups;
  const doc::Project& p = *ctx_.doc();
  for (const doc::DeviceId id : selectedDevices()) {
    const doc::DeviceLoc l = doc::findDevice(p, id);
    if (l.device == nullptr) continue;
    const doc::DeviceId g = doc::isGroupDevice(*l.device) ? id : l.device->group;
    if (g.valid() && std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
  }
  if (groups.empty()) {
    ctx_.setStatus("Ungroup: select a group (or a device inside one)");
    return false;
  }
  bool any = false;
  for (const doc::DeviceId g : groups) any = ungroup(g) || any;
  return any;
}

bool ProjectPatchView::setDeviceGroup(doc::DeviceId device, doc::DeviceId group) {
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), device);
  if (l.device == nullptr) return false;
  if (l.device->group == group) return false;
  return apply(doc::ops::setDeviceGroup(*ctx_.doc(), ctx_.store().ids(), ctx_.deviceLibrary(), device, group),
               group.valid() ? "Into Group" : "Out of Group");
}

// ---- GU.2: copy / paste of blocks, group presets ------------------------------------------------------------------

bool ProjectPatchView::copyBlocks(std::span<const doc::DeviceId> devices) {
  if (devices.empty()) {
    ctx_.setStatus("Copy: select blocks first");
    return false;
  }
  Result<doc::BlockSet> set = doc::ops::copyBlocks(*ctx_.doc(), devices);
  if (!set) {
    ctx_.setStatus(set.error().message);
    return false;
  }
  const size_t n = set->devices.size();
  ctx_.setBlockClipboard(std::move(*set));
  ctx_.setStatus("Copied " + std::to_string(n) + (n == 1 ? " block" : " blocks") + " (Ctrl+V pastes at the pointer)");
  return true;
}

bool ProjectPatchView::copySelection() {
  std::vector<doc::DeviceId> d = selectedDevices();
  if (selectedGroup_.valid() && std::find(d.begin(), d.end(), selectedGroup_) == d.end()) d.push_back(selectedGroup_);
  if (scope_.valid()) std::erase(d, scope_);  // a group panel never copies its own group (its frame is not a block there)
  if (d.empty()) {
    if (scope_.valid()) return false;
    ctx_.setStatus("Copy: select blocks first (click, Ctrl+click or drag a box around them)");
    return false;
  }
  return copyBlocks(d);
}

bool ProjectPatchView::pasteAt(graph::Point2 at) {
  sync();
  const doc::BlockSet& clip = ctx_.blockClipboard();
  if (clip.empty()) {
    ctx_.setStatus("Paste: nothing copied (Ctrl+C copies the selected blocks)");
    return false;
  }
  doc::TrackId track;
  doc::DeviceId group;
  uint32_t band = kNone;
  if (!placeTarget(at, track, group, band)) {
    ctx_.setStatus("Paste: there is no track to place it on");
    return false;
  }
  doc::ops::EditResult e = doc::ops::pasteBlocks(*ctx_.doc(), ctx_.store().ids(), track, clip, group);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  // The top-level blocks of the copy come first in Edit::devices (the rest are inside them).
  size_t count = 0;
  for (const doc::DevicePtr& d : clip.devices) count += d->group.valid() ? 0 : 1;
  const std::vector<doc::DeviceId> top(e->devices.begin(), e->devices.begin() + static_cast<std::ptrdiff_t>(std::min(count, e->devices.size())));
  if (!apply(e, "Paste")) return false;
  // Each at the pointer, a step apart (the overlap rule then gives them room).
  for (size_t i = 0; i < top.size(); ++i)
    placeAt(top[i], {at.x + 24.0f * static_cast<float>(i), at.y + 24.0f * static_cast<float>(i)}, {}, {});
  canvas_->clearSelection();
  if (top.size() == 1 && doc::isGroupDevice(*doc::findDevice(*ctx_.doc(), top.front()).device)) selectGroup(top.front());
  ctx_.setStatus("Pasted " + std::to_string(e->devices.size()) + (e->devices.size() == 1 ? " block" : " blocks"));
  return true;
}

bool ProjectPatchView::saveGroupPreset(doc::DeviceId group) {
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), group);
  if (l.device == nullptr || !doc::isGroupDevice(*l.device)) {
    ctx_.setStatus("Save as Group Preset: not a group");
    return false;
  }
  const doc::DeviceId one[1] = {group};
  Result<doc::BlockSet> set = doc::ops::copyBlocks(*ctx_.doc(), one);
  if (!set) {
    ctx_.setStatus(set.error().message);
    return false;
  }
  Result<std::string> path = ctx_.library().saveGroupPreset(*set, l.device->name.empty() ? std::string("Group") : l.device->name);
  if (!path) {
    ctx_.setStatus("Save as Group Preset: " + path.error().message);
    return false;
  }
  ctx_.setStatus("Saved group preset " + *path);
  return true;
}

void ProjectPatchView::setGroupOpen(doc::DeviceId group, bool open) {
  const bool now = groupOpen(group);
  if (now == open) return;
  if (open) std::erase(closedGroups_, group);
  else closedGroups_.push_back(group);
  if (!open && selectedGroup_ == group) selectedGroup_ = {};
  relayout();
}

bool ProjectPatchView::groupOpen(doc::DeviceId group) const noexcept {
  return std::find(closedGroups_.begin(), closedGroups_.end(), group) == closedGroups_.end();
}

void ProjectPatchView::selectGroup(doc::DeviceId group) {
  if (group.valid() && canvas_->scene()) canvas_->clearSelection();  // one selection: the group alone
  if (selectedGroup_ == group) return;
  selectedGroup_ = group;
  canvas_->markDirty();
}

bool ProjectPatchView::toggleMute(doc::TrackId track) {
  const doc::TrackId one[1] = {track};
  return apply(doc::ops::toggleTracksMute(*ctx_.doc(), one), "Track Activator");
}

bool ProjectPatchView::solo(doc::TrackId track, bool additive) {
  return apply(doc::ops::soloTrack(*ctx_.doc(), track, additive), "Solo");
}

void ProjectPatchView::beginValueGesture(const char* label) {
  if (valueGesture_) return;
  valueGesture_ = ctx_.store().beginGesture(label).ok();
}

void ProjectPatchView::setVolume(doc::TrackId track, float db) { (void)ctx_.store().dispatch(doc::SetTrackVolume{track, db}); }
void ProjectPatchView::setPan(doc::TrackId track, float pan) { (void)ctx_.store().dispatch(doc::SetTrackPan{track, pan}); }

std::vector<ui::MenuItem> ProjectPatchView::panLawItems(doc::TrackId track) {
  std::vector<ui::MenuItem> items;
  const doc::Track* t = ctx_.doc() ? doc::findTrackPtr(*ctx_.doc(), track) : nullptr;
  if (t == nullptr) return items;
  for (const doc::PanMode m : doc::kPanModes) {
    ViewContext* cx = &ctx_;
    ui::MenuItem it = ui::MenuItem::item(std::string(doc::panModeName(m)), [cx, track, m] {
      if (Status st = cx->store().dispatch(doc::SetTrackPanMode{track, m}); !st) cx->setStatus("Pan Law: " + st.error().message);
    });
    it.checked = t->mixer.panMode == m;
    items.push_back(std::move(it));
  }
  return items;
}

ui::Menu* ProjectPatchView::openPanLawMenu(doc::TrackId track, Vec2 at) {
  std::vector<ui::MenuItem> items = panLawItems(track);
  if (items.empty()) return nullptr;
  return ui::openContextMenu(*canvas_, at, std::move(items));
}
void ProjectPatchView::setSend(doc::TrackId track, doc::TrackId ret, float db) {
  (void)ctx_.store().dispatch(doc::SetSendLevel{track, ret, db});
}

void ProjectPatchView::endValueGesture() {
  if (!valueGesture_) return;
  valueGesture_ = false;
  (void)ctx_.store().commitGesture();
}

void ProjectPatchView::cancelValueGesture() {
  if (!valueGesture_) return;
  valueGesture_ = false;
  (void)ctx_.store().cancelGesture();
}

void ProjectPatchView::nudge(uint32_t entity, float dx, float dy) { nudge(std::span<const uint32_t>(&entity, 1), dx, dy); }

void ProjectPatchView::nudge(std::span<const uint32_t> entities, float dx, float dy) {
  if ((dx == 0.0f && dy == 0.0f) || entities.empty()) return;
  for (const uint32_t entity : entities) {
    doc::PatchNudge& n = ctx_.store().view().patchNudges[entity];
    n.dx = std::round(n.dx + dx);
    n.dy = std::round(n.dy + dy);
    if (n.empty()) ctx_.store().view().patchNudges.erase(entity);  // TX.resize: a scaled block keeps its entry
    moved_.push_back(entity);  // TX.block-drag: it gives way most if it now overlaps a block
  }
  ctx_.viewStateChanged();
  stale_ = true;
  sync();
}

void ProjectPatchView::resetLayout() {
  // TX.resize: the positions only; a block's size is the user's choice for that block (Reset Size / S + double-click).
  auto& nudges = ctx_.store().view().patchNudges;
  bool changed = false;
  for (auto it = nudges.begin(); it != nudges.end();) {
    if (it->second.dx != 0.0f || it->second.dy != 0.0f || it->second.mixerPlaced) changed = true;
    it->second.dx = it->second.dy = 0.0f;
    it->second.mixerPlaced = false;  // TX.mixer-look: back to the band's top right
    it->second.mixerX = it->second.mixerY = 0.0f;
    it = it->second.empty() ? nudges.erase(it) : std::next(it);
  }
  if (!changed) return;
  ctx_.viewStateChanged();
  stale_ = true;
  sync();
}

// ---- TX.mixer-look: a placed mixer ----------------------------------------------------------------------------------

bool ProjectPatchView::placeMixer(doc::TrackId track, graph::Point2 at) {
  const uint32_t b = patch_.bandOf(track);
  if (b == kNone || patch_.bands[b].mixer == kNone || !std::isfinite(at.x) || !std::isfinite(at.y)) return false;
  const Band& band = patch_.bands[b];
  // From the band's top-left (whole units), never left of it nor over its title strip (the layout keeps that too).
  const float rx = std::max(0.0f, std::round(at.x - band.x0)), ry = std::max(metrics::kTitle, std::round(at.y - band.y));
  auto& nudges = ctx_.store().view().patchNudges;
  doc::PatchNudge& n = nudges[track.value];
  if (n.mixerPlaced && n.mixerX == rx && n.mixerY == ry) return false;
  n.mixerPlaced = true;
  n.mixerX = rx;
  n.mixerY = ry;
  ctx_.viewStateChanged();
  stale_ = true;
  sync();
  return true;
}

bool ProjectPatchView::unplaceMixer(doc::TrackId track) {
  auto& nudges = ctx_.store().view().patchNudges;
  const auto it = nudges.find(track.value);
  if (it == nudges.end() || !it->second.mixerPlaced) return false;
  it->second.mixerPlaced = false;
  it->second.mixerX = it->second.mixerY = 0.0f;
  if (it->second.empty()) nudges.erase(it);
  ctx_.viewStateChanged();
  stale_ = true;
  sync();
  return true;
}

bool ProjectPatchView::mixerPlaced(doc::TrackId track) const noexcept {
  const auto& nudges = ctx_.store().view().patchNudges;
  const auto it = nudges.find(track.value);
  return it != nudges.end() && it->second.mixerPlaced;
}

float ProjectPatchView::blockScale(doc::DeviceId device) const noexcept {
  const auto& nudges = ctx_.store().view().patchNudges;
  const auto it = nudges.find(device.value);
  return it == nudges.end() ? 1.0f : it->second.scale;
}

float ProjectPatchView::clampBlockScale(float scale, bool snap) noexcept {
  if (!std::isfinite(scale)) return 1.0f;
  float k = std::clamp(scale, doc::kMinPatchScale, doc::kMaxPatchScale);
  if (snap) return std::clamp(std::round(k / kBlockScaleStep) * kBlockScaleStep, doc::kMinPatchScale, doc::kMaxPatchScale);
  if (std::fabs(k - 1.0f) < kBlockScaleDetent) return 1.0f;  // a detent at its own size
  return std::round(k * 100.0f) / 100.0f;                    // whole percent (tidy files, fewer re-layouts)
}

bool ProjectPatchView::setBlockScale(doc::DeviceId device, float scale) {
  const float k = clampBlockScale(scale, false);
  if (k == blockScale(device)) return false;
  auto& nudges = ctx_.store().view().patchNudges;
  doc::PatchNudge& n = nudges[device.value];
  n.scale = k;
  if (n.empty()) nudges.erase(device.value);
  ctx_.viewStateChanged();
  ++stats_.rescales;
  if (std::find(resized_.begin(), resized_.end(), device.value) == resized_.end()) resized_.push_back(device.value);
  relayout();  // model + scene again (the lowering kept: view state is not the document)
  return true;
}

// ---- TX.orbit-ports ---------------------------------------------------------------------------------------------------

bool ProjectPatchView::portPinned(doc::DeviceId device, bool input) const noexcept {
  const auto& nudges = ctx_.store().view().patchNudges;
  const auto it = nudges.find(device.value);
  return it != nudges.end() && (input ? it->second.inPinned : it->second.outPinned);
}

bool ProjectPatchView::pinPort(doc::DeviceId device, bool input, float angle) {
  if (!device.valid() || !std::isfinite(angle)) return false;
  const float a = doc::patchPinQuantise(angle);
  auto& nudges = ctx_.store().view().patchNudges;
  doc::PatchNudge& n = nudges[device.value];
  bool& pinned = input ? n.inPinned : n.outPinned;
  float& at = input ? n.inAngle : n.outAngle;
  if (pinned && at == a) return false;
  pinned = true;
  at = a;
  ctx_.viewStateChanged();
  relayout();  // model + scene again (the lowering kept: view state is not the document)
  return true;
}

bool ProjectPatchView::unpinPort(doc::DeviceId device, bool input) {
  auto& nudges = ctx_.store().view().patchNudges;
  const auto it = nudges.find(device.value);
  if (it == nudges.end() || !(input ? it->second.inPinned : it->second.outPinned)) return false;
  (input ? it->second.inPinned : it->second.outPinned) = false;
  (input ? it->second.inAngle : it->second.outAngle) = 0.0f;
  if (it->second.empty()) nudges.erase(it);
  ctx_.viewStateChanged();
  relayout();
  return true;
}

bool ProjectPatchView::deleteCable(uint32_t edge) {
  if (edge >= patch_.edges.size()) return false;
  const Edge& e = patch_.edges[edge];
  if (!e.cable || !e.wire.valid() || doc::findWire(*ctx_.doc(), e.wire) == nullptr) return false;
  const doc::WireId one[1] = {e.wire};
  return apply(doc::ops::deleteWires(*ctx_.doc(), one), "Delete Cable");
}

// ---- menus ----------------------------------------------------------------------------------------------------------

ui::Menu* ProjectPatchView::openBandMenu(uint32_t band, Vec2 at) {
  if (band >= patch_.bands.size()) return nullptr;
  const Band b = patch_.bands[band];
  std::vector<ui::MenuItem> items;
  std::vector<ui::MenuItem> add;
  if (const doc::DeviceLibrary* lib = ctx_.deviceLibrary())
    for (const doc::DeviceInfo& d : lib->catalog().devices()) {
      if (d.kind == doc::DeviceKind::Instrument && b.kind != doc::TrackKind::Midi) continue;
      const doc::TrackId track = b.track;
      const std::string patch = d.patch;
      add.push_back(ui::MenuItem::item(d.name, [this, track, patch] { (void)addDevice(track, patch); }));
    }
  // TMM.10: the meta modules, as chain devices of the band, at its end (TMM.10-fix1: any type on any band; its chain
  // input takes the last device's signal; it replaces nothing).
  if (!add.empty()) {
    add.push_back(ui::MenuItem::sep());
    ui::MenuItem head = ui::MenuItem::item("Meta Modules");
    head.enabled = false;
    add.push_back(std::move(head));
    for (const meta::Type* t : metaModuleTypes()) {
      const doc::TrackId track = b.track;
      const std::string patch = std::string(doc::kMetaDevicePrefix) + std::string(t->id);
      ui::MenuItem it = ui::MenuItem::item("   " + std::string(t->name), [this, track, patch] { (void)addDevice(track, patch); });
      it.enabled = doc::ops::metaDeviceKindAt(*ctx_.doc(), *t, track).ok();
      add.push_back(std::move(it));
    }
  }
  ui::MenuItem sub = ui::MenuItem::sub("Add Device", std::move(add));
  sub.enabled = !sub.submenu.empty();
  items.push_back(std::move(sub));
  // TX.patching (SPEC §15 2026-11-01 "free patching in the circuit"): a device placed in the band unconnected (out of
  // its series, under the chain): it plays only through the wires drawn to and from it.
  std::vector<ui::MenuItem> addFree = addFreeDeviceItems(b.track);
  ui::MenuItem freeSub = ui::MenuItem::sub("Add Free Device", std::move(addFree));
  freeSub.enabled = !freeSub.submenu.empty();
  items.push_back(std::move(freeSub));
  items.push_back(ui::MenuItem::sep());
  const doc::TrackId track = b.track;
  items.push_back(ui::MenuItem::item("Open Band", [this, track] { openBand(track); }));  // T5.x-circuit-chain
  items.push_back(ui::MenuItem::item("Zoom to Band", [this, track] { focusBand(track); }));
  items.push_back(ui::MenuItem::item("Zoom to Project", [this] { fitAll(); }, "F"));
  ui::MenuItem reset = ui::MenuItem::item("Reset Layout", [this] { resetLayout(); });
  reset.enabled = !ctx_.store().view().patchNudges.empty();
  items.push_back(std::move(reset));
  items.push_back(ui::MenuItem::sep());
  ui::MenuItem cost = ui::MenuItem::item("Show CPU Cost", [this] { setShowCosts(!showCosts_); });
  cost.checked = showCosts_;
  cost.enabled = costSource_ != nullptr;
  items.push_back(std::move(cost));
  return ui::openContextMenu(*canvas_, at, std::move(items));
}

ui::Menu* ProjectPatchView::openNodeMenu(uint32_t node, Vec2 at) {
  if (node >= patch_.nodes.size()) return nullptr;
  const Node n = patch_.nodes[node];
  std::vector<ui::MenuItem> items;
  if (n.kind == NodeKind::Device) {
    const doc::DeviceId d = n.device;
    // TX.knob-curves: over a knob / slider of the block's face or panel, its curve first.
    if (const circuit::CircuitCanvas::FaceParamHit fh = canvas_->faceParamAt(at); !fh.param.empty())
      if (std::optional<ui::MenuItem> curve = knobCurveMenu(ctx_, d, fh.param)) {
        items.push_back(std::move(*curve));
        items.push_back(ui::MenuItem::sep());
      }
    // TX.groups: Ctrl+G / Ctrl+Shift+G, and a group's capsule opened or closed.
    {
      std::vector<doc::DeviceId> sel = selectedDevices();
      if (std::find(sel.begin(), sel.end(), d) == sel.end()) sel.assign(1, d);
      // GU.2: groups nest — a group, or blocks inside a capsule, group too (the builder says why when it cannot).
      items.push_back(ui::MenuItem::item("Group", [this, sel] { (void)groupDevices(sel); }, "Ctrl+G"));
      const doc::DeviceId grp = n.group ? d : n.capsule != kNone ? patch_.nodes[n.capsule].device : doc::DeviceId{};
      if (grp.valid()) {
        items.push_back(ui::MenuItem::item("Ungroup", [this, grp] { (void)ungroup(grp); }, "Ctrl+Shift+G"));
        const bool open = groupOpen(grp);
        items.push_back(ui::MenuItem::item(open ? "Close Group" : "Open Group", [this, grp, open] { setGroupOpen(grp, !open); }));
      }
      if (n.capsule != kNone) {  // GU.2: out into the group around its capsule (or out of every group)
        const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), patch_.nodes[n.capsule].device);
        const doc::DeviceId outer = l.device != nullptr ? l.device->group : doc::DeviceId{};
        items.push_back(ui::MenuItem::item("Take Out of Group", [this, d, outer] { (void)setDeviceGroup(d, outer); }));
      }
      // GU.2: copy / paste of blocks with the wires among them; a group saved as a group preset (the node menu spawns it).
      items.push_back(ui::MenuItem::item("Copy", [this, sel] { (void)copyBlocks(sel); }, "Ctrl+C"));
      if (n.group)
        items.push_back(ui::MenuItem::item("Save as Group Preset", [this, d] { (void)saveGroupPreset(d); }));
      // GU.5: the group's panel window; a control of the block (or the block's controls) onto its group's panel.
      if (n.group && !embedded()) {
        ui::MenuItem win = ui::MenuItem::item("Panel Window", [this, d] { (void)togglePanelWindow(d); });
        win.checked = panelWindowOpen(d);
        items.push_back(std::move(win));
      }
      if (!embedded() && n.capsule != kNone) {
        const doc::Binding c = controlAt(at);
        std::vector<doc::Binding> bs = c.bound() ? std::vector<doc::Binding>{c} : controlsOf(d);
        ui::MenuItem send = ui::MenuItem::item(c.bound() ? "Send Control to Panel" : "Send Controls to Panel", [this, bs] {
          if (!bs.empty()) (void)sendToPanel(panelGroupFor(bs.front().device), bs);
        }, "P");
        send.enabled = !bs.empty();
        items.push_back(std::move(send));
      }
    }
    items.push_back(ui::MenuItem::item("Delete", [this, d] {
      const doc::DeviceId one[1] = {d};
      (void)apply(doc::ops::deleteDevices(*ctx_.doc(), one), "Delete Device");
    }, "Delete"));
    items.push_back(ui::MenuItem::item(n.active ? "Deactivate" : "Activate", [this, d] {
      const doc::DeviceId one[1] = {d};
      (void)toggleDevicesActive(one);
    }, "0"));
    // TX.all-free: off the main path (its neighbours joined; it keeps its other cables), and dissolve (it goes, the path
    // stays connected).
    if (!n.offPath)
      items.push_back(ui::MenuItem::item("Take Off Path", [this, d] {
        const doc::DeviceId one[1] = {d};
        (void)takeOffPath(one);
      }));
    items.push_back(ui::MenuItem::item("Dissolve", [this, d] { (void)dissolve(d); }, "Ctrl+Delete"));
    ui::MenuItem open = ui::MenuItem::item(deviceExpanded(d) ? "Close Atoms" : "Open to Atoms", [this, d] { setDeviceExpanded(d, !deviceExpanded(d)); });
    open.enabled = !n.missing;
    items.push_back(std::move(open));
    // T5.x-one-circuit: its band opened here, framed on this device (racks too); the project-device circuit editor
    // (T4.4-fix1: edits go into the document; editing a stock device forks it).
    const doc::TrackId bandTrackId = patch_.bands[n.band].track;
    ui::MenuItem circuit = ui::MenuItem::item("Open in Circuit View", [this, d, bandTrackId] { (void)openCircuit(bandTrackId, d); });
    items.push_back(std::move(circuit));
    if (!n.rack) {
      ui::MenuItem edit = ui::MenuItem::item("Edit Device Circuit", [this, d] { (void)editDeviceCircuit(d); });
      edit.enabled = ctx_.deviceLibrary() != nullptr && !n.missing && !n.meta;  // a meta module's atoms are generated
      items.push_back(std::move(edit));
    }
    if (n.edited)
      items.push_back(ui::MenuItem::item("Revert to Stock", [this, d] {
        const doc::DeviceId one[1] = {d};
        (void)apply(doc::ops::revertDevicesToStock(*ctx_.doc(), one), "Revert to Stock");
      }));
    const auto& nudges0 = ctx_.store().view().patchNudges;
    const auto own = nudges0.find(d.value);
    ui::MenuItem reset = ui::MenuItem::item("Reset Position", [this, d] {
      auto& nudges = ctx_.store().view().patchNudges;
      const auto it = nudges.find(d.value);
      if (it == nudges.end()) return;
      it->second.dx = it->second.dy = 0.0f;  // TX.resize: its size stays
      if (it->second.empty()) nudges.erase(it);
      ctx_.viewStateChanged();
      stale_ = true;
      sync();
    });
    reset.enabled = own != nudges0.end() && (own->second.dx != 0.0f || own->second.dy != 0.0f);
    items.push_back(std::move(reset));
    // TX.resize: S-drag scales a block; this (or S + double-click) gives it its own size again.
    ui::MenuItem size = ui::MenuItem::item("Reset Size", [this, d] { (void)setBlockScale(d, 1.0f); });
    size.enabled = !n.expanded && blockScale(d) != 1.0f;
    items.push_back(std::move(size));
  } else if (n.kind == NodeKind::Mixer) {
    const doc::TrackId t = patch_.bands[n.band].track;
    items.push_back(ui::MenuItem::sub("Pan Law", panLawItems(t)));  // TX.pan-laws
    items.push_back(ui::MenuItem::item(mixerExpanded(t) ? "Collapse Mixer" : "Expand Mixer", [this, t] { setMixerExpanded(t, !mixerExpanded(t)); }));
    // TX.mixer-look: a placed mixer back where the layout puts it (the band's top right).
    ui::MenuItem home = ui::MenuItem::item("Mixer to Top Right", [this, t] { (void)unplaceMixer(t); });
    home.enabled = mixerPlaced(t);
    items.push_back(std::move(home));
    // Output ▸: the master, the groups (not the track itself or its members), Sends Only.
    std::vector<ui::MenuItem> outs;
    outs.push_back(ui::MenuItem::item("Master", [this, t] {
      if (const ProjectPatch& pp = patch_; pp.master != kNone) (void)routeTo(t, pp.bands[pp.master].track);
    }));
    const uint32_t self = n.band;
    for (uint32_t i = 0; i < patch_.bands.size(); ++i) {
      const Band& g = patch_.bands[i];
      if (g.kind != doc::TrackKind::Group || i == self) continue;
      if (patch_.bands[self].kind == doc::TrackKind::Group && i > self && i <= patch_.bands[self].lastMember) continue;
      const doc::TrackId gt = g.track;
      outs.push_back(ui::MenuItem::item(g.name, [this, t, gt] { (void)routeTo(t, gt); }));
    }
    outs.push_back(ui::MenuItem::item("Sends Only", [this, t] {
      (void)dispatch(doc::Batch{"Route Output", {doc::SetTrackOutput{t, doc::Output{doc::OutputKind::None, {}}}}}, "Route");
    }));
    if (patch_.bands[self].kind != doc::TrackKind::Master) items.push_back(ui::MenuItem::sub("Output", std::move(outs)));
  } else {
    return openBandMenu(n.band, at);
  }
  return ui::openContextMenu(*canvas_, at, std::move(items));
}

ui::Menu* ProjectPatchView::openWireMenu(uint32_t edge, Vec2 at) {
  if (edge >= patch_.edges.size()) return nullptr;
  const Edge& e = patch_.edges[edge];
  const char* label = nullptr;
  switch (e.kind) {
    case EdgeKind::Modulation: label = "Delete Wire"; break;
    case EdgeKind::Send: label = e.silent ? nullptr : "Send to -inf"; break;
    case EdgeKind::Route: label = "Sends Only"; break;
    default: break;
  }
  if (label == nullptr) return nullptr;
  std::vector<ui::MenuItem> items;
  const Band from = patch_.bands[patch_.nodes[e.from].band];
  const Band to = patch_.bands[patch_.nodes[e.to].band];
  const EdgeKind kind = e.kind;
  const doc::WireId wire = e.wire;
  items.push_back(ui::MenuItem::item(label, [this, kind, wire, from, to] {
    switch (kind) {
      case EdgeKind::Modulation: {
        const doc::WireId one[1] = {wire};
        (void)apply(doc::ops::deleteWires(*ctx_.doc(), one), "Delete Wire");
        break;
      }
      case EdgeKind::Send: (void)dispatch(doc::Batch{"Send Off", {doc::SetSendLevel{from.track, to.track, doc::kMinVolumeDb}}}, "Send"); break;
      case EdgeKind::Route:
        (void)dispatch(doc::Batch{"Route Output", {doc::SetTrackOutput{from.track, doc::Output{doc::OutputKind::None, {}}}}}, "Route");
        break;
      default: break;
    }
  }, "Delete"));
  return ui::openContextMenu(*canvas_, at, std::move(items));
}

ui::Menu* ProjectPatchView::openParamMenu(doc::DeviceId from, const std::string& port, uint32_t toNode, Vec2 at,
                                          doc::TrackId notes) {
  if (toNode >= patch_.nodes.size() || patch_.nodes[toNode].kind != NodeKind::Device) return nullptr;
  const Node& n = patch_.nodes[toNode];
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc loc = doc::findDevice(p, n.device);
  std::shared_ptr<const doc::ResolvedDevice> hold;  // a user device's params are its own text's (T4.4-fix1)
  const doc::DeviceInfo* info = lib != nullptr && loc.device != nullptr ? lib->infoOf(*loc.device, hold) : nullptr;
  if (info == nullptr) {
    ctx_.setStatus("Wire: no parameters known for " + n.title);
    return nullptr;
  }
  std::vector<ui::MenuItem> items;
  const doc::DeviceId to = n.device;
  for (const doc::DeviceParamInfo& prm : info->params) {
    if (prm.kind == graph::Kind::Bool) continue;  // bool params are not modulation targets (T4.7)
    const std::string name = prm.name;
    items.push_back(ui::MenuItem::item(prm.label.empty() ? prm.name : prm.label, [this, from, port, to, name, notes] {
      (void)(notes.valid() ? connectNotes(notes, port, to, name) : connect(from, port, to, name));  // TMM.10-fix2
    }));
  }
  // TX.patching: its inputs too (an audio input: the signal adds to the chain's; a meta module's frequency or phase
  // modulation input). A meta device's are its type's real inputs (the expansion has an `in` for one only once wired).
  std::vector<std::pair<std::string, std::string>> inputs;  // (name, label)
  if (n.face != nullptr || n.meta) {
    if (const meta::Type* t = meta::findType(doc::metaDeviceType(*loc.device)))
      for (const meta::InputSpec& in : t->inputs)
        if (in.kind == graph::Kind::Real) inputs.emplace_back(std::string(in.name), in.label.empty() ? std::string(in.name) : std::string(in.label));
  } else if (!n.rack) {
    for (const Port& q : n.inputs)
      if (!q.param && q.name != "notes") inputs.emplace_back(q.name, q.label.empty() ? std::string("Audio In") : q.label);
  }
  if (!inputs.empty() && !items.empty()) items.push_back(ui::MenuItem::sep());
  for (const auto& [name, label] : inputs)
    items.push_back(ui::MenuItem::item("Input: " + label, [this, from, port, to, name, notes] {
      (void)(notes.valid() ? connectNotes(notes, port, to, name) : connect(from, port, to, name));
    }));
  if (items.empty()) return nullptr;
  return ui::openContextMenu(*canvas_, at, std::move(items));
}

// ---- readouts -------------------------------------------------------------------------------------------------------

void ProjectPatchView::setMeterSource(MeterSource* source) { meters_ = source; }

void ProjectPatchView::updateReadout(uint32_t edge) {
  if (edge == readout_.edge) return;
  readout_ = {};
  readout_.edge = edge;
  if (readoutTimer_) {
    cancelTimer(readoutTimer_);
    readoutTimer_ = 0;
  }
  if (edge == kNone || edge >= patch_.edges.size()) {
    canvas_->markDirty();
    return;
  }
  pollReadout();
}

void ProjectPatchView::onTimer(uint32_t id) {
  if (id == playheadTimer_) {
    playheadTimer_ = 0;
    readPlayhead();
    return;
  }
  if (id == costTimer_) {
    costTimer_ = 0;
    pollCosts();
    return;
  }
  if (id != readoutTimer_) return;
  readoutTimer_ = 0;
  pollReadout();
}

// ---- cost labels ----------------------------------------------------------------------------------------------------

void ProjectPatchView::setCostSource(DeviceCostSource* source) {
  if (source == costSource_) return;
  if (costSource_ != nullptr && showCosts_) costSource_->setCostMetering(false);
  costSource_ = source;
  if (costSource_ != nullptr && showCosts_ && shown()) {
    costSource_->setCostMetering(true);
    pollCosts();
  }
}

void ProjectPatchView::setShowCosts(bool on) {
  if (on == showCosts_) return;
  showCosts_ = on;
  if (costSource_ != nullptr) costSource_->setCostMetering(on && shown());
  if (!on) {
    if (costTimer_) cancelTimer(costTimer_);
    costTimer_ = 0;
    costs_.clear();
    canvas_->markDirty();
    return;
  }
  pollCosts();
}

void ProjectPatchView::pollCosts() {
  if (!showCosts_ || costSource_ == nullptr || !shown()) return;
  std::vector<doc::DeviceId> ids;
  std::vector<uint32_t> nodes;
  for (uint32_t n = 0; n < patch_.nodes.size(); ++n)
    if (patch_.nodes[n].kind == NodeKind::Device) {
      ids.push_back(patch_.nodes[n].device);
      nodes.push_back(n);
    }
  std::vector<float> ns(ids.size(), -1.0f);
  costSource_->readDeviceCosts(ids, ns);
  std::vector<float> next(patch_.nodes.size(), -1.0f);
  bool changed = costs_.size() != next.size();
  for (size_t i = 0; i < nodes.size(); ++i) {
    const float prev = nodes[i] < costs_.size() ? costs_[nodes[i]] : -1.0f;
    // Smoothed like the circuit editor's labels (a reading every 250 ms jitters).
    next[nodes[i]] = ns[i] < 0.0f ? -1.0f : (prev < 0.0f ? ns[i] : prev + 0.35f * (ns[i] - prev));
    changed = changed || std::fabs(next[nodes[i]] - prev) > 0.05f * std::max(1.0f, prev);
  }
  costs_ = std::move(next);
  if (changed) canvas_->markDirty();
  if (costSource_->costLive()) costTimer_ = requestTimer(kCostPollSeconds);
}


void ProjectPatchView::pollReadout() {
  const uint32_t edge = readout_.edge;
  if (edge == kNone || edge >= patch_.edges.size()) return;
  const Edge& e = patch_.edges[edge];
  const Band& from = patch_.bands[patch_.nodes[e.from].band];
  const Band& to = patch_.bands[patch_.nodes[e.to].band];
  const doc::Project& p = *ctx_.doc();
  bool live = false;
  auto level = [&](doc::TrackId t) -> std::string {
    if (meters_ == nullptr) return "no meter";
    const doc::TrackId ids[1] = {t};
    ui::MeterLevels lv[1];
    meters_->read(ids, lv);
    live = meters_->live();
    const float peak = std::max(lv[0].peakDb[0], lv[0].channels > 1 ? lv[0].peakDb[1] : -200.0f);
    if (!(peak > -120.0f)) return "silent";
    char buf[48];
    std::snprintf(buf, sizeof buf, "peak %.1f dB", static_cast<double>(peak));
    return buf;
  };
  switch (e.kind) {
    case EdgeKind::Notes:
      readout_.title = from.name + " notes";
      readout_.value = "per voice, into the instrument";
      break;
    case EdgeKind::Chain:
      readout_.title = from.name + ": " + patch_.nodes[e.from].title + " \xE2\x86\x92 " + patch_.nodes[e.to].title;
      readout_.value = "audio";
      break;
    case EdgeKind::Route:
      readout_.title = from.name + " \xE2\x86\x92 " + to.name;
      readout_.value = level(from.track);
      break;
    case EdgeKind::Send: {
      float db = doc::kMinVolumeDb;
      if (const doc::Track* t = doc::findTrackPtr(p, from.track))
        for (const doc::Send& s : t->mixer.sends)
          if (s.target == to.track) db = s.levelDb;
      readout_.title = "Send " + from.name + " \xE2\x86\x92 " + to.name + (e.pre ? " (pre)" : " (post)");
      readout_.value = "send " + formatVolume(db) + " dB" + (e.delayed ? ", 1-sample delay" : "") + " \xC2\xB7 track " + level(from.track);
      break;
    }
    case EdgeKind::Modulation: {
      const doc::Wire* w = doc::findWire(p, e.wire);
      const Node& tn = patch_.nodes[e.to];
      const doc::NoteOutput* no = w != nullptr && w->fromNoteSource() ? doc::findNoteOutput(w->port) : nullptr;
      // TMM.10-fix2: a note output reads as "<track> notes: Hz → …" (the note source has no device name).
      const std::string source = no != nullptr ? from.name + " notes: " + std::string(no->label)
                                               : patch_.nodes[e.from].title + "." + patch_.nodes[e.from].outputs[e.fromPort].name;
      const std::string& toLabel = tn.inputs[e.toPort].label.empty() ? tn.inputs[e.toPort].name : tn.inputs[e.toPort].label;
      readout_.title = source + " \xE2\x86\x92 " + tn.title + "." + toLabel;
      char buf[96];
      // TX.patching: an added wire has no amount; a migrated one keeps its depth. GU.1: an Add wire adds in position
      // (before a curved control's curve) or in the curve's units (a linear one: dB for a dB gain); a Units wire after it.
      enum class Adds : uint8_t { Travel, KnobTravel, Units, Db, Input, Legacy } adds = Adds::Travel;
      if (w != nullptr && w->law == doc::WireLaw::Units) {
        adds = Adds::Legacy;
      } else if (w != nullptr && !w->travels()) {
        adds = Adds::Input;
        const doc::DeviceLoc tl = doc::findDevice(p, w->to);
        std::shared_ptr<const doc::ResolvedDevice> hold;
        const doc::DeviceLibrary* devlib = ctx_.deviceLibrary();
        const graph::Patch* callee =
            tl.device != nullptr && devlib != nullptr && !tl.device->rack ? devlib->patchOf(*tl.device, hold) : nullptr;
        if (const graph::ParamDecl* decl = callee != nullptr ? doc::findParamDecl(*callee, w->param) : nullptr) {
          const dsp::Curve c = doc::controlCurve(*tl.device, w->param, *decl);
          adds = !doc::addsInUnits(c) ? Adds::KnobTravel : c.output == dsp::CurveOutput::DbToGain ? Adds::Db : Adds::Units;
        } else if (tl.device != nullptr && tl.device->rack) {
          adds = Adds::Units;
        }
      }
      switch (adds) {
        case Adds::Travel: std::snprintf(buf, sizeof buf, "amount %+.0f %%", w != nullptr ? w->amount * 100.0 : 0.0); break;
        case Adds::KnobTravel: std::snprintf(buf, sizeof buf, "adds in knob position (1 = the whole range, through its curve)"); break;
        case Adds::Units: std::snprintf(buf, sizeof buf, "adds in the knob's units"); break;
        case Adds::Db: std::snprintf(buf, sizeof buf, "adds in dB"); break;
        case Adds::Input: std::snprintf(buf, sizeof buf, "adds to the input"); break;
        case Adds::Legacy: std::snprintf(buf, sizeof buf, "adds in the param's units after its curve (older project)"); break;
      }
      readout_.value = buf;
      if (no != nullptr) {
        static constexpr const char* kReducers[] = {"sum", "last", "highest", "lowest", "loudest"};
        readout_.value += std::string(" \xC2\xB7 per voice (into a global param: ") + kReducers[static_cast<size_t>(w->reducer)] + ")";
      }
      if (e.delayed) readout_.value += ", z\xE2\x81\xBB\xC2\xB9";
      if (e.status == doc::WireStatus::Inactive) readout_.value += ", inactive";
      if (e.status == doc::WireStatus::Refused) readout_.value += ", refused (a loop the engine cannot run)";
      if (ModulationSource* src = ctx_.modulationSource(); src != nullptr && w != nullptr) {
        const ModTarget t[1] = {{w->to, w->param}};
        ModSweep s[1];
        src->readModulation(t, s);
        live = src->modulationLive();
        if (s[0].known && adds == Adds::Db) {  // TX.knob-curves: the tap is in nepers
          constexpr double kDbPerNeper = 20.0 / std::numbers::ln10;
          std::snprintf(buf, sizeof buf, " \xC2\xB7 moved +%.3g / -%.3g dB", static_cast<double>(s[0].up) * kDbPerNeper,
                        static_cast<double>(s[0].down) * kDbPerNeper);
          readout_.value += buf;
        } else if (s[0].known && (adds == Adds::Units || adds == Adds::Legacy || adds == Adds::Input)) {
          // TX.patching: an added wire's tap is in the param's units
          std::snprintf(buf, sizeof buf, " \xC2\xB7 moved +%.4g / -%.4g", static_cast<double>(s[0].up), static_cast<double>(s[0].down));
          readout_.value += buf;
        } else if (s[0].known) {
          std::snprintf(buf, sizeof buf, " \xC2\xB7 swept +%.0f %% / -%.0f %%", static_cast<double>(s[0].up) * 100.0,
                        static_cast<double>(s[0].down) * 100.0);
          readout_.value += buf;
        }
      }
      break;
    }
  }
  canvas_->markDirty();
  if (live) readoutTimer_ = requestTimer(1.0 / 30.0);
}

}  // namespace pq::views::project_patch

// ---- install --------------------------------------------------------------------------------------------------------

namespace pq::views {

project_patch::ProjectPatchView& installProjectPatch(Shell& shell) {
  ViewContext& ctx = shell.context();
  auto& view = shell.slot(ShellSlot::Patch).add<project_patch::ProjectPatchView>(ctx);
  MixerView* mixer = findMixer(shell);
  view.hideOthers = [mixer] {
    if (mixer != nullptr && mixer->shown()) mixer->setShown(false);
  };
  ctx.setViewAction("view.togglePatch", [&view] { view.toggle(); });
  // T5.x-one-circuit: every "Open in Circuit View" opens this view on the device's band; Ctrl+Alt+C on the selected track.
  ctx.setCircuitOpener([&view](doc::TrackId track, doc::DeviceId device) { return view.openCircuit(track, device); });
  ctx.setViewAction("view.trackCircuit", [&ctx] { (void)ctx.openProjectCircuit(ctx.selectedTrack()); });
  // A clip opened from a lane window: its editor takes the keyboard (as a double-click in the arrangement does).
  view.onOpenClip = [&shell](const doc::ClipRef& ref) {
    const doc::ClipSlot* slot = nullptr;
    if (const doc::Track* t = doc::findTrackPtr(*shell.context().doc(), ref.track))
      for (const doc::ClipSlot& c : t->clips)
        if (c.id == ref.clip) slot = &c;
    shell.tree().layoutNow();
    if (ui::Widget* w = shell.slot(ShellSlot::ClipDetail).find(slot != nullptr && slot->isAudio() ? "audio-clip-view" : "clip-view/notes"))
      shell.tree().setFocus(w);
  };
  if (mixer != nullptr)  // the mixer view takes the track area from the patch view too
    ctx.setViewAction("view.toggleMixer", [&view, mixer] {
      if (view.shown()) view.setShown(false, false);
      mixer->toggle();
    });
  return view;
}

project_patch::ProjectPatchView* findProjectPatch(const Shell& shell) noexcept {
  return dynamic_cast<project_patch::ProjectPatchView*>(shell.slot(ShellSlot::Patch).find("project-patch"));
}

}  // namespace pq::views
