// The project patch view's canvas (T4.13): the bands and group containers under the circuit, the clip strips, mixer
// strips, badges and readouts over it, and the edit gestures. See pq/views/project_patch/project_patch_view.h.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../common/drag.h"
#include "../common/paint.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/theme.h"
#include "pq/views/circuit/probes.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

float pinY(const Node& n, size_t i) noexcept {
  switch (n.kind) {
    case NodeKind::Sum: return 16.0f;
    case NodeKind::Output: return 8.0f;
    default: return metrics::kPinTop + metrics::kPinPitch * static_cast<float>(i);
  }
}

bool overlaps(const Rect& r, const ui::IRect& clip) noexcept {
  return r.x1 >= static_cast<float>(clip.x) && r.x0 <= static_cast<float>(clip.right()) && r.y1 >= static_cast<float>(clip.y) &&
         r.y0 <= static_cast<float>(clip.bottom());
}

ui::IRect toIRect(const Rect& r) noexcept {
  const auto x0 = static_cast<int32_t>(std::floor(r.x0)), y0 = static_cast<int32_t>(std::floor(r.y0));
  return {x0, y0, static_cast<int32_t>(std::ceil(r.x1)) - x0, static_cast<int32_t>(std::ceil(r.y1)) - y0};
}

Rect snapRect(Rect r) noexcept { return {std::round(r.x0), std::round(r.y0), std::round(r.x1), std::round(r.y1)}; }

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1; }

}  // namespace

ProjectCanvas::ProjectCanvas(ProjectPatchView& view) : CircuitCanvas("patch-canvas"), view_(view) {
  onHoverChanged = [this](const circuit::CanvasHit& hit) {
    uint32_t edge = kNone;
    const ProjectScene& ps = view_.scene();
    if (hit.kind == circuit::CanvasHit::Kind::Wire && hit.index < ps.modelEdge.size()) edge = ps.modelEdge[hit.index];
    view_.updateReadout(edge);
  };
}

Rect ProjectCanvas::deviceRect(float x, float y, float w, float h) const {
  const Vec2 a = toDevice({x, y}), b = toDevice({x + w, y + h});
  return {a.x, a.y, b.x, b.y};
}

ProjectCanvas::PinHit ProjectCanvas::pinAt(Vec2 d) const {
  const ProjectPatch& pp = view_.patch();
  const graph::Point2 w = toWorld(d);
  const float s = deviceScale();
  const float reach = 8.0f * (tree() ? tree()->scale() : 1.0f) / std::max(s, 1e-6f);
  PinHit best;
  float bestD = reach;
  for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
    const Node& n = pp.nodes[i];
    if (w.x < n.x - reach || w.x > n.x + n.w + reach || w.y < n.y - reach || w.y > n.y + n.h + reach) continue;
    for (int side = 0; side < 2; ++side) {
      const auto& ports = side == 0 ? n.inputs : n.outputs;
      const float px = side == 0 ? n.x : n.x + n.w;
      for (size_t k = 0; k < ports.size(); ++k) {
        const float dd = std::max(std::fabs(w.x - px), std::fabs(w.y - (n.y + pinY(n, k))));
        if (dd <= bestD) {
          bestD = dd;
          best = {i, static_cast<uint32_t>(k), side == 0};
        }
      }
    }
  }
  return best;
}

uint32_t ProjectCanvas::nodeAt(Vec2 d) const {
  const ProjectPatch& pp = view_.patch();
  const graph::Point2 w = toWorld(d);
  for (uint32_t i = static_cast<uint32_t>(pp.nodes.size()); i-- > 0;) {
    const Node& n = pp.nodes[i];
    if (w.x >= n.x && w.x <= n.x + n.w && w.y >= n.y && w.y <= n.y + n.h) return i;
  }
  return kNone;
}

uint32_t ProjectCanvas::bandAt(Vec2 d) const {
  const ProjectPatch& pp = view_.patch();
  const graph::Point2 w = toWorld(d);
  for (uint32_t i = static_cast<uint32_t>(pp.bands.size()); i-- > 0;) {
    const Band& b = pp.bands[i];
    if (w.y >= b.y && w.y <= b.y + b.h && w.x >= b.x0 - 8.0f) return i;
  }
  return kNone;
}

// Mixer strip parts (world offsets from the node's top-left; see the file comment of project_patch_view.h).
ProjectCanvas::MixerAreas ProjectCanvas::mixerAreas(uint32_t node) const {
  MixerAreas a;
  const ProjectPatch& pp = view_.patch();
  if (node >= pp.nodes.size()) return a;
  const Node& n = pp.nodes[node];
  auto r = [&](float x, float y, float w, float h) { return deviceRect(n.x + x, n.y + y, w, h); };
  a.mute = r(10.0f, 20.0f, 12.0f, 11.0f);
  a.solo = r(24.0f, 20.0f, 12.0f, 11.0f);
  if (n.expanded) {
    a.pan = r(40.0f, 22.0f, 24.0f, 7.0f);
    a.volume = r(10.0f, 36.0f, 54.0f, 7.0f);
    for (size_t k = 1; k < n.outputs.size(); ++k)
      a.sends.push_back(r(72.0f, pinY(n, k) - 3.0f, n.w - 72.0f - 22.0f, 6.0f));
  } else {
    const float right = n.outputs.size() > 1 ? 22.0f : 10.0f;
    a.pan = r(40.0f, 22.0f, n.w - 40.0f - right, 7.0f);
    a.volume = r(10.0f, 36.0f, n.w - 10.0f - right, 7.0f);
  }
  return a;
}

// ---- painting -------------------------------------------------------------------------------------------------------

void ProjectCanvas::paintUnderlay(ui::PaintContext& ctx) {
  const ui::Theme& th = *ctx.res.theme;
  const ProjectPatch& pp = view_.patch();
  ui::DrawList& dl = ctx.dl;
  const ui::IRect clip = ui::intersect(ctx.clip, rect());
  const float s = deviceScale();
  const float us = ctx.scale.factor;
  const float bar = std::max(2.0f, std::round(3.0f * us));
  // Group containers first (outer before inner), then the band strips.
  for (const Band& b : pp.bands) {
    if (b.kind != doc::TrackKind::Group) continue;
    const Rect r = snapRect(deviceRect(b.cx0, b.cy0, b.cx1 - b.cx0, b.cy1 - b.cy0));
    if (!overlaps(r, clip)) continue;
    const Rgba col = ui::hex(b.colour);
    dl.addRectFilled(r, ui::lerp(th.circuit.background, col, b.active ? 0.07f : 0.03f));
    dl.addRectStroke(r, ui::mulAlpha(col, b.active ? 0.8f : 0.35f), std::max(1.0f, std::round(us)));
  }
  const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  for (const Band& b : pp.bands) {
    const Rect r = snapRect(deviceRect(b.x0, b.y, b.x1 - b.x0, b.h));
    if (!overlaps(r, clip)) continue;
    const Rgba col = ui::hex(b.colour);
    const doc::Track* t = view_.bandTrack(static_cast<uint32_t>(&b - pp.bands.data()));
    const bool dim = !b.active || (t != nullptr && t->mixer.mute);
    dl.addRectFilled(r, ui::lerp(th.circuit.background, col, dim ? 0.05f : 0.12f));
    dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, dim ? ui::mulAlpha(col, 0.4f) : col);
    // The name in the title strip, at every zoom where the strip is readable (the overview's labels).
    const float stripH = metrics::kTitle * s;
    if (names) {
      const ui::FontId font = ctx.res.uiFonts->small;
      const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
      if (r.y1 - r.y0 >= px + 2.0f) {
        const float h = std::clamp(stripH, px + 2.0f, r.y1 - r.y0);
        const ui::IRect box{static_cast<int32_t>(r.x0 + bar + 6.0f), static_cast<int32_t>(r.y0),
                            static_cast<int32_t>(std::min(r.x1 - r.x0 - bar - 10.0f, 360.0f * us)), static_cast<int32_t>(h)};
        std::string label = b.name;
        if (!b.active) label += "  (deactivated)";
        paint::text(ctx, font, box, label, dim ? th.circuit.pinText : th.circuit.symbolText, ui::TextAlign::Left);
      }
    }
  }
  // Expanded devices: the frame their atoms are drawn in.
  for (const Node& n : pp.nodes) {
    if (n.kind != NodeKind::Device || !n.expanded) continue;
    if (n.band < pp.bands.size() && view_.scene().sceneNode.size() > 0) {
      const uint32_t mi = static_cast<uint32_t>(&n - pp.nodes.data());
      if (view_.scene().sceneNode[mi] != kNone) continue;  // not opened (no inner scene)
    }
    const Rect r = snapRect(deviceRect(n.x, n.y, n.w, n.h));
    if (!overlaps(r, clip)) continue;
    dl.addRectFilled(r, ui::lerp(th.circuit.background, th.circuit.subcircuitFill, 0.5f));
    dl.addRectFilled({r.x0, r.y0, r.x1, r.y0 + std::round(16.0f * s)}, th.circuit.subcircuitTitle);
    dl.addRectStroke(r, th.circuit.symbolStroke, std::max(1.0f, std::round(circuit::strokeWidth(s))));
    if (names && lod() != Lod::Far) {
      const ui::IRect box{static_cast<int32_t>(r.x0 + 6.0f), static_cast<int32_t>(r.y0), static_cast<int32_t>(r.x1 - r.x0 - 12.0f),
                          static_cast<int32_t>(std::round(16.0f * s))};
      paint::text(ctx, ctx.res.uiFonts->small, box, n.title + "  (atoms, read-only)", th.circuit.symbolText, ui::TextAlign::Left);
    }
  }
}

void ProjectCanvas::paintOverlay(ui::PaintContext& ctx) {
  const ui::Theme& th = *ctx.res.theme;
  const ProjectPatch& pp = view_.patch();
  const ProjectScene& ps = view_.scene();
  ui::DrawList& dl = ctx.dl;
  const ui::IRect clip = ui::intersect(ctx.clip, rect());
  const float s = deviceScale();
  const float us = ctx.scale.factor;
  const Lod level = lod();
  const bool text = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr && level == Lod::Near;
  const double end = static_cast<double>(std::max<Ticks>(view_.songEnd(), 1));

  if (level != Lod::Far) {
    for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
      const Node& n = pp.nodes[i];
      if (i < ps.sceneNode.size() && ps.sceneNode[i] == kNone) continue;  // expanded: its atoms are drawn
      const Rect nr = deviceRect(n.x, n.y, n.w, n.h);
      if (!overlaps(nr, clip)) continue;
      const Band& band = pp.bands[n.band];
      if (n.kind == NodeKind::Source) {
        // The lane's clips as a strip: song time left → right.
        const Rect strip = snapRect(deviceRect(n.x + 12.0f, n.y + 20.0f, n.w - 24.0f, n.h - 28.0f));
        dl.addRectFilled(strip, th.arrangement.lane);
        if (const doc::Track* t = view_.bandTrack(n.band)) {
          const float w = strip.x1 - strip.x0;
          for (const doc::ClipSlot& c : t->clips) {
            const float x0 = strip.x0 + static_cast<float>(static_cast<double>(c.clip->start) / end) * w;
            const float x1 = strip.x0 + static_cast<float>(static_cast<double>(c.clip->start + c.clip->length) / end) * w;
            if (x0 >= strip.x1) break;
            const Rgba col = c.clip->active && !t->mixer.mute ? ui::hex(c.clip->colour.rgb) : th.circuit.pinText;
            dl.addRectFilled({std::round(x0), strip.y0 + 1.0f, std::max(std::round(x0) + 1.0f, std::round(std::min(x1, strip.x1))), strip.y1 - 1.0f}, col);
          }
        }
      } else if (n.kind == NodeKind::Mixer) {
        const doc::Track* t = view_.bandTrack(n.band);
        if (t == nullptr) continue;
        const MixerAreas a = mixerAreas(i);
        const bool master = t->kind == doc::TrackKind::Master;
        if (!master) {
          paint::box(dl, toIRect(snapRect(a.mute)), 0.0f, t->mixer.mute ? th.mixer.buttonOff : th.mixer.activatorOn, th.mixer.boxBorder);
          paint::box(dl, toIRect(snapRect(a.solo)), 0.0f, t->mixer.solo ? th.mixer.soloOn : th.mixer.buttonOff, th.mixer.boxBorder);
        }
        // Pan: a centre tick and the position.
        const Rect pr = snapRect(a.pan);
        dl.addRectFilled(pr, th.mixer.field);
        const float cx = std::round((pr.x0 + pr.x1) * 0.5f);
        const float px = std::round(cx + (pr.x1 - pr.x0) * 0.5f * std::clamp(t->mixer.pan, -1.0f, 1.0f));
        dl.addRectFilled({std::min(cx, px), pr.y0, std::max(cx, px) + 1.0f, pr.y1}, th.mixer.panFill);
        // Volume: a horizontal fader (Live's fader law).
        const Rect vr = snapRect(a.volume);
        dl.addRectFilled(vr, th.mixer.field);
        const float pos = static_cast<float>(volumeToFader(t->mixer.volumeDb));
        dl.addRectFilled({vr.x0, vr.y0, std::round(vr.x0 + (vr.x1 - vr.x0) * pos), vr.y1}, th.mixer.volumeFill);
        // Sends (expanded strips): one bar per send pin.
        for (size_t k = 0; k < a.sends.size() && k + 1 < n.outputs.size(); ++k) {
          const Rect sr = snapRect(a.sends[k]);
          dl.addRectFilled(sr, th.mixer.field);
          float db = doc::kMinVolumeDb;
          const std::string& port = n.outputs[k + 1].name;
          for (const doc::Send& sd : t->mixer.sends)
            if ("s" + std::to_string(sd.target.value) == port) db = sd.levelDb;
          const float sp = static_cast<float>(volumeToFader(db));
          dl.addRectFilled({sr.x0, sr.y0, std::round(sr.x0 + (sr.x1 - sr.x0) * sp), sr.y1}, th.mixer.sendFill);
        }
        if (text) {
          const Rect vb0 = deviceRect(n.x + 10.0f, n.y + 45.0f, (n.expanded ? 64.0f : n.w - 10.0f) - 10.0f, 12.0f);
          const ui::IRect vb = toIRect(vb0);
          paint::text(ctx, ctx.res.uiFonts->small, vb, formatVolume(t->mixer.volumeDb) + " dB  " + formatPan(t->mixer.pan),
                      th.circuit.pinText, ui::TextAlign::Left);
        }
        if (text && !master) {
          paint::text(ctx, ctx.res.uiFonts->small, toIRect(a.mute), "M", th.mixer.activatorText, ui::TextAlign::Center);
          paint::text(ctx, ctx.res.uiFonts->small, toIRect(a.solo), "S", th.mixer.buttonText, ui::TextAlign::Center);
        }
      } else if (n.kind == NodeKind::Device && (!n.lowered || !n.active || !band.active)) {
        // Bypassed (deactivated, missing, on a deactivated track): veiled; the chain wire passes under it.
        const Rect r = snapRect(nr);
        dl.addRectFilled(r, ui::mulAlpha(th.circuit.background, 0.55f));
        if (text) {
          const char* why = n.missing ? "missing" : (!n.active ? "off" : "inactive");
          const ui::IRect box{static_cast<int32_t>(r.x0), static_cast<int32_t>(r.y1 - std::round(14.0f * s)), static_cast<int32_t>(r.x1 - r.x0),
                              static_cast<int32_t>(std::round(12.0f * s))};
          paint::text(ctx, ctx.res.uiFonts->small, box, why, th.circuit.pinText, ui::TextAlign::Center);
        }
      }
    }
    // Titles at the middle level of detail (the canvas writes them only from the near level, in the title band, where
    // they do not fit yet): a device's name at the top of its body (its pin labels are not drawn at this level), the
    // MIDI source's above its clip strip. Mixer strips are known by their controls.
    if (level == Lod::Mid && ctx.res.uiFonts != nullptr && ctx.res.text != nullptr) {
      const ui::FontId font = ctx.res.uiFonts->small;
      const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
      for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
        const Node& n = pp.nodes[i];
        if (n.kind != NodeKind::Device && n.kind != NodeKind::Source) continue;
        if (i < ps.sceneNode.size() && ps.sceneNode[i] == kNone) continue;
        const float top = n.kind == NodeKind::Source ? 1.0f : 18.0f;
        const float room = (n.kind == NodeKind::Source ? 19.0f : n.h - 20.0f) * s;
        if (room < px + 2.0f) continue;
        const Rect r = deviceRect(n.x + 8.0f, n.y + top, n.w - 16.0f, (px + 2.0f) / s);
        if (!overlaps(r, clip)) continue;
        paint::text(ctx, font, toIRect(r), n.title, th.circuit.symbolText, ui::TextAlign::Center);
      }
    }
    // z⁻¹ on wires with an automatic 1-sample delay (SPEC §15 2026-10-24).
    if (text)
      for (const Edge& e : pp.edges) {
        if (!e.delayed || e.points.size() < 2) continue;
        const graph::Point2 a = e.points[e.points.size() / 2 - (e.points.size() % 2 == 0 ? 1 : 0)];
        const Vec2 c = toDevice(a);
        const ui::IRect box{static_cast<int32_t>(c.x) - static_cast<int32_t>(14.0f * us), static_cast<int32_t>(c.y) - static_cast<int32_t>(7.0f * us),
                            static_cast<int32_t>(28.0f * us), static_cast<int32_t>(14.0f * us)};
        if (!overlaps(box.toRect(), clip)) continue;
        paint::box(dl, box, 3.0f * us, th.circuit.costBg, th.circuit.cost);
        paint::text(ctx, ctx.res.uiFonts->small, box, "z\xE2\x81\xBB\xC2\xB9", th.circuit.cost, ui::TextAlign::Center);
      }
  }

  // CPU cost labels (Show CPU Cost): ns per sample above each device block's right end.
  if (view_.showCosts() && level != Lod::Far && ctx.res.uiFonts != nullptr && ctx.res.text != nullptr) {
    ui::TextSystem& ts = *ctx.res.text;
    const ui::FontId font = ctx.res.uiFonts->small;
    const int32_t lh = static_cast<int32_t>(std::round(14.0f * us)), pad = static_cast<int32_t>(std::round(4.0f * us));
    for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
      const float ns = view_.deviceCost(i);
      if (ns < 0.0f || pp.nodes[i].kind != NodeKind::Device) continue;
      const Node& n = pp.nodes[i];
      const Vec2 tr = toDevice({n.x + n.w, n.y});
      const std::string label = circuit::formatCostNs(static_cast<double>(ns));
      const int32_t w = ts.shape(font, label).advance + 2 * pad;
      const ui::IRect box{static_cast<int32_t>(tr.x) - w, static_cast<int32_t>(tr.y) - lh - 1, w, lh};
      if (!overlaps(box.toRect(), clip)) continue;
      paint::box(dl, box, 3.0f * us, th.circuit.costBg, 0);
      paint::text(ctx, font, box, label, th.circuit.cost, ui::TextAlign::Center);
    }
  }

  // Gestures.
  if (drag_ == Drag::Wire && from_.valid()) {
    const Node& n = pp.nodes[from_.node];
    const Vec2 a = toDevice({from_.input ? n.x : n.x + n.w, n.y + pinY(n, from_.port)});
    const float t = std::max(1.0f, std::round(2.0f * us));
    const float mx = std::round((a.x + last_.x) * 0.5f);
    dl.addHLine(std::min(a.x, mx), std::max(a.x, mx), std::round(a.y), th.circuit.selected, t);
    dl.addVLine(mx, std::min(a.y, last_.y), std::max(a.y, last_.y), th.circuit.selected, t);
    dl.addHLine(std::min(mx, last_.x), std::max(mx, last_.x), std::round(last_.y), th.circuit.selected, t);
    const uint32_t target = nodeAt(last_);
    if (target != kNone && target != from_.node) {
      const Node& tn = pp.nodes[target];
      dl.addRectStroke(snapRect(deviceRect(tn.x, tn.y, tn.w, tn.h)), th.circuit.selected, std::max(1.0f, std::round(2.0f * us)));
    }
  }
  if (drag_ == Drag::MoveDevice && dragNode_ != kNone) {
    const Node& n = pp.nodes[dragNode_];
    const Rect r = deviceRect(n.x, n.y, n.w, n.h);
    const float dx = last_.x - press_.x, dy = last_.y - press_.y;
    dl.addRectStroke(snapRect({r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}), th.circuit.selected, std::max(1.0f, std::round(us)));
    const uint32_t b = bandAt(last_);
    if (b != kNone) {
      const Band& band = pp.bands[b];
      const graph::Point2 w = toWorld(last_);
      float x = band.x0 + 8.0f;
      for (const uint32_t d : band.devices) {
        if (d == dragNode_) continue;
        const Node& dn = pp.nodes[d];
        if (dn.x + dn.w * 0.5f < w.x) x = dn.x + dn.w + metrics::kNodeGap * 0.5f;
      }
      const Rect m = deviceRect(x - 1.0f, band.y + 4.0f, 2.0f, band.h - 8.0f);
      dl.addRectFilled(snapRect(m), th.circuit.selected);
    }
  }
  if (drag_ == Drag::Nudge) {
    const float dx = last_.x - press_.x, dy = last_.y - press_.y;
    Rect r{};
    if (dragNode_ != kNone) {
      const Node& n = pp.nodes[dragNode_];
      r = deviceRect(n.x, n.y, n.w, n.h);
    } else if (dragBand_ != kNone) {
      const Band& b = pp.bands[dragBand_];
      r = deviceRect(b.x0, b.y, b.x1 - b.x0, b.h);
    }
    dl.addRectStroke(snapRect({r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}), th.circuit.hover, std::max(1.0f, std::round(us)));
  }

  // The hovered wire's readout, at the pointer.
  const ProjectPatchView::Readout& ro = view_.readout();
  if (ro.edge != kNone && drag_ == Drag::None && ctx.res.uiFonts != nullptr && ctx.res.text != nullptr) {
    ui::TextSystem& ts = *ctx.res.text;
    const ui::FontId font = ctx.res.uiFonts->small;
    const int32_t w1 = ts.shape(font, ro.title).advance, w2 = ts.shape(font, ro.value).advance;
    const int32_t pad = static_cast<int32_t>(6.0f * us), lh = static_cast<int32_t>(std::round(15.0f * us));
    const int32_t bw = std::max(w1, w2) + 2 * pad, bh = 2 * lh + pad;
    int32_t x = static_cast<int32_t>(pointer_.x) + static_cast<int32_t>(14.0f * us), y = static_cast<int32_t>(pointer_.y) + static_cast<int32_t>(14.0f * us);
    x = std::min(x, rect().right() - bw - 2);
    y = std::min(y, rect().bottom() - bh - 2);
    const ui::IRect box{x, y, bw, bh};
    paint::box(dl, box, 3.0f * us, th.circuit.probeBg, th.circuit.probeGrid);
    paint::text(ctx, font, {x + pad, y + pad / 2, bw - 2 * pad, lh}, ro.title, th.circuit.probeText, ui::TextAlign::Left);
    paint::text(ctx, font, {x + pad, y + pad / 2 + lh, bw - 2 * pad, lh}, ro.value, th.circuit.probeTrace, ui::TextAlign::Left);
  }
}

// ---- input ----------------------------------------------------------------------------------------------------------

bool ProjectCanvas::onPointer(const ui::PointerEvent& e) {
  ProjectPatchView& v = view_;
  const ProjectPatch& pp = v.patch();
  const float us = tree() ? tree()->scale() : 1.0f;
  if (e.type == ui::PointerType::Move || e.type == ui::PointerType::Down) {
    if (v.readout().edge != kNone && (e.pos.x != pointer_.x || e.pos.y != pointer_.y)) markDirty();
    pointer_ = e.pos;
  }
  if (!scene()) return CircuitCanvas::onPointer(e);
  switch (e.type) {
    case ui::PointerType::Down: {
      if (e.button == ui::PointerButton::Right) {
        if (tree()) tree()->setFocus(this);
        const circuit::CanvasHit hit = hitTest(e.pos);
        if (hit.kind == circuit::CanvasHit::Kind::Wire && hit.index < v.scene().modelEdge.size() && v.scene().modelEdge[hit.index] != kNone) {
          if (v.openWireMenu(v.scene().modelEdge[hit.index], e.pos) != nullptr) return true;
        }
        if (const uint32_t n = nodeAt(e.pos); n != kNone) {
          (void)v.openNodeMenu(n, e.pos);
          return true;
        }
        if (const uint32_t b = bandAt(e.pos); b != kNone) {
          (void)v.openBandMenu(b, e.pos);
          return true;
        }
        return true;
      }
      if (e.button != ui::PointerButton::Left || (e.mods & ui::mod::kAlt)) return CircuitCanvas::onPointer(e);
      if (tree()) tree()->setFocus(this);
      const uint32_t node = nodeAt(e.pos);
      if (e.clickCount >= 2) {
        if (node != kNone && pp.nodes[node].kind == NodeKind::Device && !pp.nodes[node].missing) {
          const Node& n = pp.nodes[node];
          // An opened device closes on a double-click on its frame (not on its atoms).
          const bool onAtoms = n.expanded && hitTest(e.pos).kind == circuit::CanvasHit::Kind::Node;
          if (!onAtoms) v.setDeviceExpanded(n.device, !n.expanded);
          return true;
        }
        if (node != kNone && pp.nodes[node].kind == NodeKind::Mixer) {
          const doc::TrackId t = pp.bands[pp.nodes[node].band].track;
          v.setMixerExpanded(t, !v.mixerExpanded(t));
          return true;
        }
        if (node == kNone) {
          if (const uint32_t b = bandAt(e.pos); b != kNone) v.focusBand(pp.bands[b].track);
          else v.fitAll();
          return true;
        }
        return true;
      }
      press_ = last_ = e.pos;
      // Mixer strip controls.
      if (node != kNone && pp.nodes[node].kind == NodeKind::Mixer) {
        const MixerAreas a = mixerAreas(node);
        const doc::TrackId t = pp.bands[pp.nodes[node].band].track;
        const doc::Track* tr = v.bandTrack(pp.nodes[node].band);
        if (tr != nullptr && tr->kind != doc::TrackKind::Master && inside(a.mute, e.pos)) {
          (void)v.toggleMute(t);
          return true;
        }
        if (tr != nullptr && tr->kind != doc::TrackKind::Master && inside(a.solo, e.pos)) {
          (void)v.solo(t, (e.mods & ui::mod::kCtrl) != 0);
          return true;
        }
        if (tr != nullptr && inside(a.volume, e.pos)) {
          drag_ = Drag::Volume;
          dragNode_ = node;
          dragStart_ = volumeToFader(tr->mixer.volumeDb);
          return true;
        }
        if (tr != nullptr && inside(a.pan, e.pos)) {
          drag_ = Drag::Pan;
          dragNode_ = node;
          dragStart_ = tr->mixer.pan;
          return true;
        }
        for (size_t k = 0; tr != nullptr && k < a.sends.size(); ++k)
          if (inside(a.sends[k], e.pos)) {
            drag_ = Drag::Send;
            dragNode_ = node;
            dragSend_ = static_cast<uint32_t>(k + 1);
            float db = doc::kMinVolumeDb;
            for (const doc::Send& sd : tr->mixer.sends)
              if ("s" + std::to_string(sd.target.value) == pp.nodes[node].outputs[k + 1].name) db = sd.levelDb;
            dragStart_ = volumeToFader(db);
            return true;
          }
      }
      // An output pin: a new wire (a device's export → a param; a mixer's out / send → a Σ).
      if (const PinHit pin = pinAt(e.pos); pin.valid() && !pin.input) {
        const Node& n = pp.nodes[pin.node];
        if (n.kind == NodeKind::Device || n.kind == NodeKind::Mixer) {
          drag_ = Drag::Wire;
          from_ = pin;
          wireCursor_ = ui::Cursor::Crosshair;
          markDirty();
          return true;
        }
      }
      // A band's title strip: nudge the band.
      if (node == kNone) {
        if (const uint32_t b = bandAt(e.pos); b != kNone) {
          const graph::Point2 w = toWorld(e.pos);
          if (w.y <= pp.bands[b].y + metrics::kTitle) {
            drag_ = Drag::Nudge;
            dragBand_ = b;
            dragNode_ = kNone;
            return true;
          }
        }
      }
      if (node != kNone && pp.nodes[node].kind == NodeKind::Device) {
        if (e.mods & ui::mod::kCtrl) {  // Ctrl+drag nudges the block; a Ctrl+click toggles its selection (on Up)
          drag_ = Drag::Nudge;
          dragNode_ = node;
          dragBand_ = kNone;
          return true;
        }
        if ((e.mods & ui::mod::kShift) == 0) {
          const uint32_t sn = node < v.scene().sceneNode.size() ? v.scene().sceneNode[node] : kNone;
          if (sn == kNone || !nodeSelected(sn)) (void)CircuitCanvas::onPointer(e);  // select it
          drag_ = Drag::MaybeMoveDevice;
          dragNode_ = node;
          return true;
        }
      }
      return CircuitCanvas::onPointer(e);
    }
    case ui::PointerType::Move: {
      if (drag_ == Drag::None) {
        hoverCursor_ = pressCursor(e.pos);  // T4.16c: what a press here would do (cursorAt returns it)
        return CircuitCanvas::onPointer(e);
      }
      last_ = e.pos;
      switch (drag_) {
        case Drag::Wire:
          wireCursor_ = wireRefusal(e.pos) != nullptr ? ui::Cursor::NotAllowed : ui::Cursor::Crosshair;
          markDirty();
          break;
        case Drag::MoveDevice:
        case Drag::Nudge: markDirty(); break;
        case Drag::MaybeMoveDevice:
          if (std::max(std::fabs(e.pos.x - press_.x), std::fabs(e.pos.y - press_.y)) >= drag::kClickSlop * us) {
            drag_ = Drag::MoveDevice;
            markDirty();
          }
          break;
        case Drag::Volume:
        case Drag::Pan:
        case Drag::Send: {
          const MixerAreas a = mixerAreas(dragNode_);
          const Rect& r = drag_ == Drag::Volume ? a.volume : (drag_ == Drag::Pan ? a.pan : a.sends[dragSend_ - 1]);
          const double width = std::max(4.0f, r.x1 - r.x0);
          const Band& band = pp.bands[pp.nodes[dragNode_].band];
          const double fine = (e.mods & ui::mod::kShift) ? 0.1 : 1.0;
          const double dx = (e.pos.x - press_.x) * fine;
          if (drag_ == Drag::Volume) {
            v.beginValueGesture("Set Volume");
            v.setVolume(band.track, static_cast<float>(faderToVolume(std::clamp(dragStart_ + dx / width, 0.0, 1.0))));
          } else if (drag_ == Drag::Pan) {
            v.beginValueGesture("Set Pan");
            v.setPan(band.track, static_cast<float>(std::clamp(dragStart_ + 2.0 * dx / width, -1.0, 1.0)));
          } else {
            const std::string& port = pp.nodes[dragNode_].outputs[dragSend_].name;
            const doc::TrackId ret{static_cast<uint32_t>(std::stoul(port.substr(1)))};
            v.beginValueGesture("Set Send Level");
            v.setSend(band.track, ret, static_cast<float>(faderToVolume(std::clamp(dragStart_ + dx / width, 0.0, 1.0))));
          }
          break;
        }
        case Drag::None: break;
      }
      return true;
    }
    case ui::PointerType::Up: {
      const Drag d = drag_;
      drag_ = Drag::None;
      if (d == Drag::None) return CircuitCanvas::onPointer(e);
      last_ = e.pos;
      switch (d) {
        case Drag::Wire: finishWire(e.pos); break;
        case Drag::MoveDevice: finishDeviceMove(e.pos); break;
        case Drag::MaybeMoveDevice: {
          // A click on a selected block selects only it.
          const uint32_t sn = dragNode_ < v.scene().sceneNode.size() ? v.scene().sceneNode[dragNode_] : kNone;
          if (sn != kNone) {
            const uint32_t one[1] = {sn};
            select(one, {});
          }
          break;
        }
        case Drag::Nudge: {
          const float s = deviceScale();
          const float dx = (e.pos.x - press_.x) / s, dy = (e.pos.y - press_.y) / s;
          const bool moved = std::max(std::fabs(e.pos.x - press_.x), std::fabs(e.pos.y - press_.y)) >= drag::kClickSlop * us;
          if (moved && dragNode_ != kNone) v.nudge(pp.nodes[dragNode_].device.value, dx, dy);
          else if (moved && dragBand_ != kNone) v.nudge(pp.bands[dragBand_].track.value, dx, dy);
          else if (dragNode_ != kNone) {  // Ctrl+click: toggle the block's selection
            const uint32_t sn = dragNode_ < v.scene().sceneNode.size() ? v.scene().sceneNode[dragNode_] : kNone;
            if (sn != kNone) {
              Selection s2 = selection();
              auto it = std::lower_bound(s2.nodes.begin(), s2.nodes.end(), sn);
              if (it != s2.nodes.end() && *it == sn) s2.nodes.erase(it);
              else s2.nodes.insert(it, sn);
              select(s2.nodes, s2.wires);
            }
          }
          break;
        }
        case Drag::Volume:
        case Drag::Pan:
        case Drag::Send: v.endValueGesture(); break;
        case Drag::None: break;
      }
      dragNode_ = dragBand_ = kNone;
      from_ = {};
      hoverCursor_ = pressCursor(e.pos);
      markDirty();
      return true;
    }
    case ui::PointerType::Leave:
      view_.updateReadout(kNone);
      hoverCursor_ = ui::Cursor::Arrow;
      return CircuitCanvas::onPointer(e);
    default: return CircuitCanvas::onPointer(e);
  }
}

const char* ProjectCanvas::wireRefusal(Vec2 pos) const {
  const ProjectPatch& pp = view_.patch();
  if (!from_.valid() || from_.node >= pp.nodes.size()) return nullptr;
  const Node& a = pp.nodes[from_.node];
  if (a.kind == NodeKind::Device)
    return from_.port == 0 ? "Wire: drag an export (the device's jacks under its output) onto a parameter" : nullptr;
  if (a.kind != NodeKind::Mixer) return nullptr;
  const uint32_t to = nodeAt(pos);
  const uint32_t band = to != kNone ? pp.nodes[to].band : bandAt(pos);
  if (band == kNone) return nullptr;
  const Band& tb = pp.bands[band];
  const std::string_view port = a.outputs[from_.port].name;
  if (port == "out")
    return tb.kind == doc::TrackKind::Midi ? "Route: a track's output goes to a group, a return (its send) or the master" : nullptr;
  // A send pin goes onto its return ("s<return id>").
  const bool itsReturn = tb.kind == doc::TrackKind::Return && port.size() > 1 && port[0] == 's' &&
                         port.substr(1) == std::to_string(tb.track.value);
  return itsReturn ? nullptr : "Send: drop it on its return's input";
}

ui::Cursor ProjectCanvas::cursorAt(Vec2 pos) const {
  switch (drag_) {
    case Drag::Wire: return wireCursor_;
    case Drag::MoveDevice: return bandAt(pos) == kNone ? ui::Cursor::NotAllowed : ui::Cursor::Move;
    case Drag::MaybeMoveDevice:
    case Drag::Nudge: return ui::Cursor::Move;
    case Drag::Volume:
    case Drag::Pan:
    case Drag::Send: return ui::Cursor::ResizeEW;
    case Drag::None: break;
  }
  if (panning() || !scene()) return CircuitCanvas::cursorAt(pos);
  return hoverCursor_;
}

ui::Cursor ProjectCanvas::pressCursor(Vec2 pos) const {
  // The order of onPointer's left press (Alt and the middle button pan: the canvas's).
  const ProjectPatch& pp = view_.patch();
  const uint32_t node = nodeAt(pos);
  if (node != kNone && pp.nodes[node].kind == NodeKind::Mixer) {
    const MixerAreas a = mixerAreas(node);
    const doc::Track* tr = view_.bandTrack(pp.nodes[node].band);
    if (tr != nullptr && tr->kind != doc::TrackKind::Master && (inside(a.mute, pos) || inside(a.solo, pos))) return ui::Cursor::Pointer;
    if (tr != nullptr && (inside(a.volume, pos) || inside(a.pan, pos))) return ui::Cursor::ResizeEW;
    for (const Rect& r : a.sends)
      if (tr != nullptr && inside(r, pos)) return ui::Cursor::ResizeEW;
  }
  if (const PinHit pin = pinAt(pos); pin.valid() && !pin.input &&
                                     (pp.nodes[pin.node].kind == NodeKind::Device || pp.nodes[pin.node].kind == NodeKind::Mixer))
    return ui::Cursor::Crosshair;
  if (node == kNone) {
    if (const uint32_t b = bandAt(pos); b != kNone && toWorld(pos).y <= pp.bands[b].y + metrics::kTitle) return ui::Cursor::Move;
    return ui::Cursor::Arrow;
  }
  return pp.nodes[node].kind == NodeKind::Device ? ui::Cursor::Move : ui::Cursor::Arrow;
}

void ProjectCanvas::finishWire(Vec2 pos) {
  ProjectPatchView& v = view_;
  const ProjectPatch& pp = v.patch();
  if (!from_.valid()) return;
  if (const char* why = wireRefusal(pos)) {
    v.context().setStatus(why);
    return;
  }
  const Node& a = pp.nodes[from_.node];
  const Band& fromBand = pp.bands[a.band];
  if (a.kind == NodeKind::Device) {
    const std::string port = a.outputs[from_.port].name;
    const PinHit target = pinAt(pos);
    if (target.valid() && target.input && pp.nodes[target.node].kind == NodeKind::Device && target.port > 0 &&
        !pp.nodes[target.node].instrument) {
      (void)v.connect(a.device, port, pp.nodes[target.node].device, pp.nodes[target.node].inputs[target.port].name);
      return;
    }
    if (target.valid() && target.input && pp.nodes[target.node].kind == NodeKind::Device && pp.nodes[target.node].instrument &&
        target.port > 0) {
      (void)v.connect(a.device, port, pp.nodes[target.node].device, pp.nodes[target.node].inputs[target.port].name);
      return;
    }
    const uint32_t to = nodeAt(pos);
    if (to != kNone && pp.nodes[to].kind == NodeKind::Device) (void)v.openParamMenu(a.device, port, to, pos);
    return;
  }
  if (a.kind == NodeKind::Mixer) {
    const uint32_t to = nodeAt(pos);
    uint32_t band = to != kNone ? pp.nodes[to].band : bandAt(pos);
    if (band == kNone) return;
    const Band& tb = pp.bands[band];
    if (a.outputs[from_.port].name == "out") (void)v.routeTo(fromBand.track, tb.track);
    else (void)v.sendOn(fromBand.track, tb.track);  // a send pin onto its return (wireRefusal checked it)
  }
}

void ProjectCanvas::finishDeviceMove(Vec2 pos) {
  ProjectPatchView& v = view_;
  const ProjectPatch& pp = v.patch();
  if (dragNode_ == kNone) return;
  const uint32_t b = bandAt(pos);
  if (b == kNone) return;
  const Band& band = pp.bands[b];
  const Node& moved = pp.nodes[dragNode_];
  const graph::Point2 w = toWorld(pos);
  uint32_t index = 0;
  for (const uint32_t d : band.devices) {
    if (d == dragNode_) continue;
    const Node& dn = pp.nodes[d];
    if (dn.x + dn.w * 0.5f < w.x) ++index;
  }
  const doc::Track* now = doc::findDevice(*v.context().doc(), moved.device).track;
  if (now != nullptr && now->id == band.track) {
    const int32_t at = now->deviceIndex(moved.device);
    if (at >= 0 && static_cast<uint32_t>(at) == index) return;  // dropped where it was
  }
  (void)v.moveDevice(moved.device, band.track, index);
}

bool ProjectCanvas::onKey(const ui::KeyEvent& e) {
  if (!e.down) return false;
  const ui::Mods mods = e.mods & ui::mod::kChordMask;
  if (e.key == ui::key::kEscape && drag_ != Drag::None) {
    onInteractionCancelled();
    return true;
  }
  if (mods == ui::mod::kNone && (e.key == ui::key::kDelete || e.key == ui::key::kBackspace)) {
    (void)view_.deleteSelection();
    return true;
  }
  if (mods == ui::mod::kNone && e.key == '0') {
    const std::vector<doc::DeviceId> d = view_.selectedDevices();
    if (!d.empty()) (void)view_.toggleDevicesActive(d);
    return true;
  }
  if (mods == ui::mod::kNone && e.key == 'f') {
    view_.fitAll();
    return true;
  }
  if (mods == ui::mod::kNone && e.key == ui::key::kEscape && selection().nodes.empty() && selection().wires.empty()) {
    view_.fitAll();  // Esc with nothing selected: back to the whole project (semantic zoom out)
    return true;
  }
  return CircuitCanvas::onKey(e);
}

void ProjectCanvas::onCaptureLost() {
  if (drag_ == Drag::Volume || drag_ == Drag::Pan || drag_ == Drag::Send) view_.endValueGesture();
  if (drag_ != Drag::None) markDirty();
  drag_ = Drag::None;
  dragNode_ = dragBand_ = kNone;
  CircuitCanvas::onCaptureLost();
}

void ProjectCanvas::onInteractionCancelled() {
  if (drag_ == Drag::Volume || drag_ == Drag::Pan || drag_ == Drag::Send) view_.cancelValueGesture();
  if (drag_ != Drag::None) markDirty();
  drag_ = Drag::None;
  dragNode_ = dragBand_ = kNone;
  from_ = {};
  CircuitCanvas::onInteractionCancelled();
}

ui::InfoText ProjectCanvas::info() const {
  const ProjectPatchView::Readout& ro = view_.readout();
  if (ro.edge != kNone) return {ro.title, ro.value};
  return {"Project Patch",
          "The whole project as one circuit. Drag an export onto a device to wire it, a mixer's output onto a group, return "
          "or the master to route it; double-click a band to zoom onto it, a device to open it to its atoms. Ctrl+Alt+P "
          "returns to the arrangement."};
}

}  // namespace pq::views::project_patch
