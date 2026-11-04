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
#include "internal.h"
#include "pq/views/circuit/chain_paint.h"
#include "pq/views/circuit/probes.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

float pinY(const Node& n, size_t i) noexcept { return pinOffset(n, i); }

// TMM.10-fix2: a MIDI track's lane window (its note source), whose outputs start wires.
bool isNoteSource(const ProjectPatch& pp, const Node& n) noexcept {
  return n.kind == NodeKind::Source && n.band < pp.bands.size() && pp.bands[n.band].kind == doc::TrackKind::Midi;
}
// TX.all-free: a band's source (a lane window, a Σ): its output 0 is the track's own signal (its notes, its clips, its
// sum), a wire's kMainPort source.
bool isTrackSource(const Node& n) noexcept { return n.kind == NodeKind::Source || n.kind == NodeKind::Sum; }
// Whether a press on output pin `pin` starts a wire: a device's (TX.patching: any output, its main one too), a mixer's
// (route, sends), a track's source (TX.all-free: its own signal; a MIDI track's note outputs).
bool wireSource(const ProjectPatch& pp, const Node& n) noexcept {
  (void)pp;
  return n.kind == NodeKind::Device || n.kind == NodeKind::Mixer || isTrackSource(n) ||
         (n.kind == NodeKind::Terminal && !n.outputs.empty());  // TX.groups: a group's In terminal
}
// TX.groups: a node a wire may end on (a device, or a group's Out terminal; TX.all-free: a track's mixer).
bool wireEnd(const Node& n) noexcept {
  return n.kind == NodeKind::Device || n.kind == NodeKind::Mixer || (n.kind == NodeKind::Terminal && !n.inputs.empty());
}

// TX.patching: the port of node `n`'s input pin `i` a wire dropped on it targets, or null. TX.all-free: an instrument's
// `notes` and a rack's own input are their main inputs (kMainPort: the main path's notes / audio).
const Port* wireTarget(const Node& n, uint32_t i) noexcept {
  if (!wireEnd(n) || i >= n.inputs.size()) return nullptr;
  return &n.inputs[i];
}

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1; }

// T5.x-circuit-chain: the opened band's name pill (device px; empty when none is drawn: no opened band, or its title
// strip is tall enough to hold the name). Drawn over its opened devices; their frames' title pills start right of it.
Rect openedBandPill(const ProjectCanvas& c, const ProjectPatchView& v, const ui::PaintContext& ctx) {
  const ProjectPatch& pp = v.patch();
  const uint32_t ob = v.openedBand().valid() ? pp.bandOf(v.openedBand()) : kNone;
  if (ob == kNone || ctx.res.uiFonts == nullptr || ctx.res.text == nullptr) return {};
  const ui::FontId font = ctx.res.uiFonts->small;
  const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
  if (metrics::kTitle * c.deviceScale() >= px + 2.0f) return {};
  const float us = ctx.scale.factor;
  const Band& b = pp.bands[ob];
  const Rect r = snapRect(c.deviceRect(b.x0, b.y, b.x1 - b.x0, b.h));
  const float pad = std::round(3.0f * us), lh = std::round(px + 4.0f * us), bar = std::max(2.0f, std::round(3.0f * us));
  const float w = static_cast<float>(ctx.res.text->shape(font, b.name).advance) + 2.0f * pad + bar;
  return {r.x0 + bar + 1.0f, r.y0 + 1.0f, r.x0 + bar + 1.0f + w, r.y0 + 1.0f + lh};
}

}  // namespace

ProjectCanvas::ProjectCanvas(ProjectPatchView& view) : CircuitCanvas("patch-canvas"), view_(view) {
  setPinStubsNearOnly(true);  // M4 review L13: the overview reads as blocks and wires
  setTerminalPillMinZoom(kFarZoom);  // T5.x-circuit-chain: opened devices' terminals named from Mid up
  // TMM.4: faces edit their instance in the document: a chain meta device's own params (path ""), or a meta call's
  // inside an opened device (its name path); one undo step per drag.
  faceBinder = [this](const circuit::SceneFace& f) {
    FaceBinding b;
    if (f.type == nullptr || f.owner == 0) return b;
    b.target = std::make_unique<DocTarget>(view_.context(), doc::DeviceId{f.owner}, f.path, *f.type);
    if (view_.faceVoices) b.voices = view_.faceVoices(doc::DeviceId{f.owner}, f.path);
    // TX.spectrum: a chain Spectrum device's face reads the device's analysis tap.
    if (f.path.empty()) b.analysis = [ctx = &view_.context(), id = f.owner] { return ctx->analysisFeed(doc::DeviceId{id}); };
    return b;
  };
  // TX.panel-faces: a closed stock device's block is its Device-view panel (bound to the document by the view).
  panelMaker = [this](const circuit::SceneFace& f) { return view_.makePanel(f); };
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
        if (side == 0 && ports[k].anchored) continue;  // TX.mod-all: no pin (its wire ends on the face's control)
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
  // Group containers first (outer before inner), then the band strips. TX.groups: a scoped canvas (a group's panel)
  // has no bands to draw.
  for (const Band& b : pp.bands) {
    if (pp.scoped) break;
    if (b.kind != doc::TrackKind::Group) continue;
    const Rect r = snapRect(deviceRect(b.cx0, b.cy0, b.cx1 - b.cx0, b.cy1 - b.cy0));
    if (!overlaps(r, clip)) continue;
    const Rgba col = ui::hex(b.colour);
    dl.addRectFilled(r, ui::lerp(th.circuit.background, col, b.active ? 0.07f : 0.03f));
    dl.addRectStroke(r, ui::mulAlpha(col, b.active ? 0.8f : 0.35f), std::max(1.0f, std::round(us)));
  }
  const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  for (const Band& b : pp.bands) {
    if (pp.scoped) break;
    const Rect r = snapRect(deviceRect(b.x0, b.y, b.x1 - b.x0, b.h));
    if (!overlaps(r, clip)) continue;
    const Rgba col = ui::hex(b.colour);
    const doc::Track* t = view_.bandTrack(static_cast<uint32_t>(&b - pp.bands.data()));
    const bool dim = !b.active || (t != nullptr && t->mixer.mute);
    // M4 review L13: the track's own colour, lighter (a grey blend of a dark colour read as mud): the body a light
    // alpha of it, the title strip a stronger one.
    const float stripH = metrics::kTitle * s;
    dl.addRectFilled(r, ui::lerp(th.circuit.background, col, dim ? 0.05f : 0.15f));
    if (!dim) dl.addRectFilled({r.x0, r.y0, r.x1, std::min(r.y1, std::round(r.y0 + stripH))}, ui::lerp(th.circuit.background, col, 0.30f));
    dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, dim ? ui::mulAlpha(col, 0.4f) : col);
    // The name in the title strip, at every zoom where the strip is readable (the overview's labels).
    if (names) {
      const ui::FontId font = ctx.res.uiFonts->small;
      const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
      // TMM.13-fix2 (MM review "UI and aesthetic" 3): only where the strip holds the name; thinner (zoomed out) the name
      // is a pill drawn over the band's blocks (paintOverlay), not text running down under its lane window.
      if ((stripH >= px + 2.0f || lod() == Lod::Near) && r.y1 - r.y0 >= px + 2.0f) {
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
    // T5.x-circuit-chain: the frame is a labelled boundary with the kind's accent (as in the track circuit view).
    const Rgba accent = n.rack ? th.accent.solo : n.instrument ? th.accent.primary : th.circuit.wireAudio;
    const float stripH = std::round(16.0f * s);
    dl.addRectFilled(r, ui::lerp(th.circuit.background, th.circuit.subcircuitFill, 0.5f));
    dl.addRectFilled({r.x0, r.y0, r.x1, r.y0 + stripH}, ui::lerp(th.circuit.background, accent, 0.22f));
    dl.addRectStroke(r, ui::mulAlpha(accent, 0.9f), std::max(1.0f, std::round(circuit::strokeWidth(s))));
    dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, accent);
    if (names) {
      const ui::FontId font = ctx.res.uiFonts->small;
      const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
      if (stripH >= px + 2.0f) {
        const ui::IRect box{static_cast<int32_t>(r.x0 + bar + 6.0f), static_cast<int32_t>(r.y0), static_cast<int32_t>(r.x1 - r.x0 - bar - 12.0f),
                            static_cast<int32_t>(stripH)};
        // T5.x-one-circuit: the kind after the name (as the Track-level view's boundaries had it).
        const int32_t adv = paint::text(ctx, font, box, n.title, th.circuit.symbolText, ui::TextAlign::Left);
        const int32_t gap = static_cast<int32_t>(std::round(10.0f * us));
        if (adv > 0 && adv + gap + 12 < box.w) {
          const char* kind = n.rack ? (n.instrument ? "Instrument Rack" : "Audio Effect Rack") : (n.instrument ? "Instrument" : "Audio Effect");
          paint::text(ctx, font, {box.x + adv + gap, box.y, box.w - adv - gap, box.h}, kind, th.circuit.pinText, ui::TextAlign::Left);
        }
      } else if (r.x1 - r.x0 >= 28.0f * us) {  // zoomed out: the name in a pill at the frame's corner
        const float pad = std::round(3.0f * us), lh = std::round(px + 4.0f * us);
        float x0 = r.x0 + bar + 1.0f;
        if (const Rect bp = openedBandPill(*this, view_, ctx); bp.x1 > bp.x0 && bp.y1 > r.y0 && bp.y0 < r.y0 + lh && bp.x1 > x0 && bp.x0 < r.x1)
          x0 = bp.x1 + std::round(4.0f * us);  // right of the band's name
        const ui::TextRun run = ctx.res.text->shapeTruncated(font, n.title, static_cast<int32_t>(r.x1 - x0 - 2.0f * pad - 4.0f));
        const Rect pb{x0, r.y0 + 1.0f, x0 + static_cast<float>(run.advance) + 2.0f * pad, r.y0 + 1.0f + lh};
        dl.addRoundedRectFilled(pb, 2.0f * us, ui::withAlpha(th.surface.sunken, 230));
        ui::TextSystem::drawRun(dl, run, {pb.x0 + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(pb.y0), static_cast<int32_t>(lh), static_cast<uint32_t>(px)))},
                                th.circuit.symbolText);
      }
    }
    // T5.x-circuit-chain: an opened rack's chains and devices as nested frames.
    if (n.rack)
      if (const circuit::ChainCircuit* cc = view_.rackCircuit(n.device))
        circuit::paintChainBoxes(ctx, *this, *cc, n.x + metrics::kExpandedPad - cc->scene->x0, n.y + 16.0f + metrics::kExpandedPad - cc->scene->y0);
  }
  // TX.groups: opened groups' capsules (a rounded frame with its title strip; its contents are scene nodes over it).
  for (const Node& n : pp.nodes) {
    if (!n.group || !n.groupOpen) continue;
    const Rect r = snapRect(deviceRect(n.x, n.y, n.w, n.h));
    if (!overlaps(r, clip)) continue;
    const Rgba accent = th.circuit.wireAudio;
    // TMM.13-fix2 (MM review "UI and aesthetic" 9: "a grouped capsule", the user's word): pill-round ends — a short
    // capsule is a true stadium, a tall one keeps a 28-unit radius so its contents stay inside the curve.
    const float rad = std::min(std::round(28.0f * s), (r.y1 - r.y0) * 0.5f);
    const float stripH = std::round(metrics::kCapsuleTitle * s);
    const float sw = std::max(1.0f, std::round(circuit::strokeWidth(s)));
    const bool sel = view_.selectedGroup() == n.device;
    dl.addRoundedRectFilled(r, rad, ui::lerp(th.circuit.background, th.circuit.subcircuitFill, n.active ? 0.6f : 0.3f));
    if (stripH >= 3.0f)
      dl.addRoundedRectFilled({r.x0, r.y0, r.x1, std::min(r.y1, r.y0 + std::max(stripH, rad))}, rad,
                              ui::lerp(th.circuit.background, accent, n.active ? 0.18f : 0.08f));
    dl.addRoundedRectStroke(r, rad, sel ? th.circuit.selected : ui::mulAlpha(accent, n.active ? 0.9f : 0.4f), sel ? 2.0f * sw : sw);
    if (names && stripH >= 8.0f) {
      const ui::FontId font = ctx.res.uiFonts->small;
      const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
      if (stripH >= px + 2.0f) {
        const ui::IRect box{static_cast<int32_t>(r.x0 + rad), static_cast<int32_t>(r.y0), static_cast<int32_t>(r.x1 - r.x0 - 2.0f * rad),
                            static_cast<int32_t>(stripH)};
        const int32_t adv = paint::text(ctx, font, box, n.title, th.circuit.symbolText, ui::TextAlign::Left);
        const int32_t gap = static_cast<int32_t>(std::round(10.0f * us));
        const std::string count = std::to_string(n.members.size()) + (n.members.size() == 1 ? " device" : " devices");
        if (adv > 0 && adv + gap + 12 < box.w)
          paint::text(ctx, font, {box.x + adv + gap, box.y, box.w - adv - gap, box.h}, count,
                      ui::lerp(th.circuit.pinText, th.circuit.symbolText, 0.85f), ui::TextAlign::Left);  // TMM.13-fix2: was 2.79:1
      }
    }
  }
}

// T5.x-one-circuit: the device "Open in Circuit View" framed, outlined (its frame or block; inside a rack its box).
static void paintFocus(ui::PaintContext& ctx, const ProjectCanvas& c, const ProjectPatchView& v) {
  LaneRect r;
  if (!v.focusedDevice().valid() || !v.deviceWorldRect(v.focusedDevice(), r)) return;
  const float us = ctx.scale.factor;
  const float t = std::max(2.0f, std::round(2.0f * us));
  const Rect d = snapRect(c.deviceRect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0));
  if (!overlaps(d, ui::intersect(ctx.clip, c.rect()))) return;
  ctx.dl.addRectStroke({d.x0 - t, d.y0 - t, d.x1 + t, d.y1 + t}, ctx.res.theme->circuit.selected, t);
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
  // TX.groups: a closed group's miniature of its contents (its boxes and wires in the block's body, under the title).
  for (const Node& n : pp.nodes) {
    if (!n.group || n.groupOpen || n.mini.empty()) continue;
    const Rect r = deviceRect(n.x + 8.0f, n.y + 20.0f, n.w - 16.0f, n.h - 26.0f);
    if (!overlaps(r, clip) || r.x1 - r.x0 < 12.0f || r.y1 - r.y0 < 8.0f) continue;
    // The miniature keeps its aspect, centred in the body.
    float mx0 = 1.0f, my0 = 1.0f, mx1 = 0.0f, my1 = 0.0f;
    for (const Node::MiniBox& b : n.mini) mx0 = std::min(mx0, b.x0), my0 = std::min(my0, b.y0), mx1 = std::max(mx1, b.x1), my1 = std::max(my1, b.y1);
    const float k = std::min((r.x1 - r.x0), (r.y1 - r.y0) * 3.0f);
    const float w = k, h = std::min(r.y1 - r.y0, k / 3.0f);
    const float ox = std::round((r.x0 + r.x1 - w) * 0.5f), oy = std::round((r.y0 + r.y1 - h) * 0.5f);
    auto at = [&](float u, float v) { return Vec2{ox + u * w, oy + v * h}; };
    const float lw = std::max(1.0f, std::round(us));
    for (size_t i = 0; i + 1 < n.miniWires.size(); i += 2) {
      const Vec2 a = at(n.miniWires[i].x, n.miniWires[i].y), b = at(n.miniWires[i + 1].x, n.miniWires[i + 1].y);
      const ui::Rgba wc = ui::mulAlpha(th.circuit.wireAudio, 0.8f);  // orthogonal, as the circuit's wires
      const float mx = std::round((a.x + b.x) * 0.5f);
      dl.addHLine(std::min(a.x, mx), std::max(a.x, mx), std::round(a.y), wc, lw);
      dl.addVLine(mx, std::min(a.y, b.y), std::max(a.y, b.y) + lw, wc, lw);
      dl.addHLine(std::min(mx, b.x), std::max(mx, b.x), std::round(b.y), wc, lw);
    }
    const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr && level == Lod::Near;
    for (const Node::MiniBox& b : n.mini) {
      const Vec2 a = at(b.x0, b.y0), c = at(b.x1, b.y1);
      const Rect br{a.x, a.y, std::max(a.x + 2.0f, c.x), std::max(a.y + 2.0f, c.y)};
      dl.addRectFilled(br, b.terminal ? ui::mulAlpha(th.circuit.wireAudio, 0.5f) : th.circuit.symbolFill);
      dl.addRectStroke(br, ui::mulAlpha(th.circuit.symbolStroke, 0.8f), lw);
      // TMM.13-fix2 (MM review M9): a closed group read as two empty boxes — each member's name in its box (truncated;
      // left out where even a few letters do not fit).
      if (names && !b.terminal && !b.name.empty()) {
        const ui::FontId font = ctx.res.uiFonts->small;
        const auto px = static_cast<int32_t>(ctx.res.text->metrics(font).pixelSize);
        const auto bw = static_cast<int32_t>(br.x1 - br.x0) - static_cast<int32_t>(std::round(6.0f * us));
        if (bw >= 3 * px / 2 && br.y1 - br.y0 >= static_cast<float>(px) + 2.0f) {
          const ui::TextRun run = ctx.res.text->shapeTruncated(font, b.name, bw);
          ui::TextSystem::drawRun(dl, run,
                                  {std::round((br.x0 + br.x1 - static_cast<float>(run.advance)) * 0.5f),
                                   static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(br.y0), static_cast<int32_t>(br.y1 - br.y0),
                                                                                static_cast<uint32_t>(px)))},
                                  th.circuit.symbolText);
        }
      }
    }
  }
  (void)ps;
  const bool text = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr && level == Lod::Near;
  // T5.x-circuit-chain: opened racks' deactivated chains and devices veiled.
  for (const Node& n : pp.nodes)
    if (n.rack && n.expanded)
      if (const circuit::ChainCircuit* cc = view_.rackCircuit(n.device))
        circuit::paintChainVeils(ctx, *this, *cc, n.x + metrics::kExpandedPad - cc->scene->x0, n.y + 16.0f + metrics::kExpandedPad - cc->scene->y0);
  // The opened band's name over its opened devices when its strip is too thin to show it under them (zoomed out).
  if (const Rect pb = openedBandPill(*this, view_, ctx); pb.x1 > pb.x0 && overlaps(pb, clip)) {
    const Band& b = pp.bands[pp.bandOf(view_.openedBand())];
    const ui::FontId font = ctx.res.uiFonts->small;
    const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
    const float pad = std::round(3.0f * us), bar = std::max(2.0f, std::round(3.0f * us));
    dl.addRoundedRectFilled(pb, 2.0f * us, ui::withAlpha(th.surface.sunken, 235));
    dl.addRectFilled({pb.x0, pb.y0, pb.x0 + bar, pb.y1}, ui::hex(b.colour));
    ui::TextSystem::drawRun(dl, ctx.res.text->shape(font, b.name),
                            {pb.x0 + bar + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(pb.y0), static_cast<int32_t>(pb.y1 - pb.y0), static_cast<uint32_t>(px)))},
                            th.circuit.symbolText);
  }

  // T5.x-one-circuit: the MIDI / audio sources are windows onto their lanes (every level of detail).
  paintLaneWindows(ctx, *this, view_);
  paintFocus(ctx, *this, view_);
  if (level != Lod::Far) {
    for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
      const Node& n = pp.nodes[i];
      if (i < ps.sceneNode.size() && ps.sceneNode[i] == kNone) continue;  // expanded: its atoms are drawn
      const Rect nr = deviceRect(n.x, n.y, n.w, n.h);
      if (!overlaps(nr, clip)) continue;
      const Band& band = pp.bands[n.band];
      if (n.kind == NodeKind::Source) {
        continue;  // a lane window (above) or a Σ (the symbol)
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
      } else if (n.kind == NodeKind::Device && (!n.lowered || !n.active || !band.active) && !(n.panel && band.active)) {
        // (TX.panel-faces: a panel shows its own switched-off look, as in the Device view.)
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
  }
  if (level != Lod::Near) {
    // M4 review L13: every block labelled below the near level (the canvas writes titles in the blocks from Near): its
    // name in a pill over the top of the block, as wide as the name (never cut), centred; a pill that would overlap
    // one already placed is left out (bands first in lane order, so the earlier block keeps its label).
    if (ctx.res.uiFonts != nullptr && ctx.res.text != nullptr) {
      ui::TextSystem& ts = *ctx.res.text;
      const ui::FontId font = ctx.res.uiFonts->small;
      const auto px = static_cast<float>(ts.metrics(font).pixelSize);
      const float pad = std::round(3.0f * us), lh = std::round(px + 4.0f * us);
      std::vector<Rect> placed;
      // TMM.13-fix2 (MM review "UI and aesthetic" 3): the bands' own titles first, so a block's pill never covers one (the
      // lane windows' "Notes" sat on "Keys", "Pad", "Mod"): it moves into its block's top, or is left out.
      for (const Band& b : pp.bands) {
        if (pp.scoped) break;
        const Rect br = snapRect(deviceRect(b.x0, b.y, b.x1 - b.x0, b.h));
        if (!overlaps(br, clip) || br.y1 - br.y0 < px + 2.0f) continue;
        const float bar = std::max(2.0f, std::round(3.0f * us));
        const ui::TextRun run = ts.shape(font, b.name);
        const float stripH = metrics::kTitle * deviceScale();
        if (stripH >= px + 2.0f) {  // the strip holds the name (paintUnderlay): keep pills off it
          placed.push_back({br.x0, br.y0, br.x0 + bar + 6.0f + static_cast<float>(run.advance) + 4.0f * us, br.y0 + stripH});
          continue;
        }
        // The band's name as a pill at its top-left corner, over its blocks (they would hide text under them).
        const Rect box{br.x0 + bar + 1.0f, br.y0 + 1.0f, br.x0 + bar + 1.0f + static_cast<float>(run.advance) + 2.0f * pad, br.y0 + 1.0f + lh};
        placed.push_back(box);
        dl.addRoundedRectFilled(box, 2.0f * us, ui::withAlpha(th.surface.sunken, 235));
        ui::TextSystem::drawRun(dl, run, {box.x0 + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(box.y0), static_cast<int32_t>(lh), static_cast<uint32_t>(px)))},
                                th.circuit.symbolText);
      }
      for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
        const Node& n = pp.nodes[i];
        if (i < ps.sceneNode.size() && ps.sceneNode[i] == kNone) continue;  // opened: its frame has the title
        if (n.kind == NodeKind::Sum || n.kind == NodeKind::Output || n.kind == NodeKind::Terminal) continue;  // the symbol says it
        if (n.face != nullptr || n.panel) continue;  // TMM.4: a face is label-free; TX.panel-faces: a panel has its title
        const Rect nr = deviceRect(n.x, n.y, n.w, n.h);
        if (nr.x1 - nr.x0 < 24.0f * us || !overlaps(nr, clip)) continue;
        const ui::TextRun run = ts.shape(font, n.title);
        const float w = static_cast<float>(run.advance) + 2.0f * pad;
        const float cx = std::round((nr.x0 + nr.x1) * 0.5f);
        // Inside the block's top where it is tall enough, else just above it.
        const float y0 = nr.y1 - nr.y0 >= lh + 2.0f ? std::round(nr.y0 + 1.0f) : std::round(nr.y0 - lh - 1.0f);
        Rect box{std::round(cx - w * 0.5f), y0, std::round(cx - w * 0.5f) + w, y0 + lh};
        auto isFree = [&](const Rect& b) {
          bool f = true;
          for (const Rect& q : placed) f = f && (b.x1 <= q.x0 || q.x1 <= b.x0 || b.y1 <= q.y0 || q.y1 <= b.y0);
          return f;
        };
        if (!isFree(box)) {  // over a band's title (or another pill): into the block's top instead
          box.y0 = std::round(nr.y0 + 1.0f);
          box.y1 = box.y0 + lh;
          if (!isFree(box)) continue;
        }
        placed.push_back(box);
        dl.addRoundedRectFilled(box, 2.0f * us, ui::withAlpha(th.surface.sunken, 225));
        ui::TextSystem::drawRun(dl, run, {box.x0 + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(box.y0), static_cast<int32_t>(lh), static_cast<uint32_t>(px)))},
                                th.circuit.symbolText);
      }
    }
  }
  {
    // z⁻¹ on wires with an automatic 1-sample delay (SPEC §15 2026-10-24).
    // TX.cables: on a cable, halfway along it as it hangs now (it rides the swing).
    if (text)
      for (uint32_t k = 0; k < pp.edges.size(); ++k) {
        const Edge& e = pp.edges[k];
        if (!e.delayed || e.points.size() < 2) continue;
        graph::Point2 a = e.points[e.points.size() / 2 - (e.points.size() % 2 == 0 ? 1 : 0)];
        if (e.cable)
          for (uint32_t w = 0; w < ps.modelEdge.size(); ++w)
            if (ps.modelEdge[w] == k) {
              a = cableMidpoint(w);
              break;
            }
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

  // Gestures. TX.node-menu-fix1: a new connection is drawn as the real cable (the canvas's loose cable, over the
  // blocks); here only what it would connect to: a soft ring on the port or control it snapped to, or the block whose
  // params a drop on its body would offer.
  if ((drag_ == Drag::Wire && from_.valid()) || drag_ == Drag::BackWire) {
    const std::span<const graph::Point2> lp = looseCablePoints();
    const float cw = std::clamp(6.0f * s, 1.5f * us, 16.0f * us);
    if (looseCableSnapped() && !lp.empty()) {
      const Vec2 c = toDevice(lp.back());
      const ui::Rgba ring = th.circuit.selected;
      dl.addCircleFilled(c, 1.9f * cw + 3.0f * us, ui::mulAlpha(ring, 0.16f));
      dl.addCircleStroke(c, 1.9f * cw + 3.0f * us, ui::mulAlpha(ring, 0.85f), std::max(1.0f, std::round(1.5f * us)));
    } else if (drag_ == Drag::Wire && wireRefusal(last_) == nullptr) {
      const uint32_t target = nodeAt(last_);
      if (target != kNone && target != from_.node && pp.nodes[target].kind == NodeKind::Device) {
        const Node& tn = pp.nodes[target];
        dl.addRoundedRectStroke(deviceRect(tn.x - 2.0f, tn.y - 2.0f, tn.w + 4.0f, tn.h + 4.0f), 4.0f * us, ui::mulAlpha(th.circuit.selected, 0.7f),
                                std::max(1.0f, std::round(1.5f * us)));
      }
    }
  }
  if (drag_ == Drag::Splice && dragNode_ != kNone && dragNode_ < pp.nodes.size()) {
    const Node& n = pp.nodes[dragNode_];
    const Rect r = deviceRect(n.x, n.y, n.w, n.h);
    const float dx = last_.x - press_.x, dy = last_.y - press_.y;
    dl.addRectStroke(snapRect({r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}), spliceEdge_ != kNone ? th.circuit.selected : th.circuit.hover,
                     std::max(1.0f, std::round((spliceEdge_ != kNone ? 2.0f : 1.0f) * us)));
  }
  if (drag_ == Drag::MoveDevice && dragNode_ != kNone) {
    const Node& n = pp.nodes[dragNode_];
    const Rect r = deviceRect(n.x, n.y, n.w, n.h);
    const float dx = last_.x - press_.x, dy = last_.y - press_.y;
    dl.addRectStroke(snapRect({r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}), th.circuit.selected, std::max(1.0f, std::round(us)));
    // TX.all-free: a block moves where it is dropped (no slot: its cables are its place in the signal path).
  }
  if (drag_ == Drag::Resize) {  // TX.resize: the block's outline and its scale at its bottom-right corner
    const uint32_t rn = pp.nodeOf(resizeDevice_);
    if (rn != kNone) {
      const Node& n = pp.nodes[rn];
      const Rect r = snapRect(deviceRect(n.x, n.y, n.w, n.h));
      dl.addRectStroke(r, th.circuit.selected, std::max(1.0f, std::round(us)));
      if (ctx.res.uiFonts != nullptr && ctx.res.text != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d %%", static_cast<int>(std::lround(n.scale * 100.0f)));
        const ui::FontId font = ctx.res.uiFonts->small;
        const int32_t tw = ctx.res.text->shape(font, buf).advance;
        const int32_t pad = static_cast<int32_t>(5.0f * us), lh = static_cast<int32_t>(std::round(15.0f * us));
        const ui::IRect box{static_cast<int32_t>(r.x1) - tw - 2 * pad, static_cast<int32_t>(r.y1) + 4, tw + 2 * pad, lh};
        paint::box(dl, box, 3.0f * us, th.circuit.probeBg, th.circuit.probeGrid);
        paint::text(ctx, font, {box.x + pad, box.y, tw, lh}, buf, th.circuit.probeText, ui::TextAlign::Left);
      }
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

  // TX.node-menu-fix1: while the node menu is open, where its choice goes: a small ring and dot in the accent.
  if (const std::optional<graph::Point2>& mk = view_.spawnMark()) {
    const Vec2 c = toDevice(*mk);
    const ui::Rgba a = th.face.signal;
    dl.setBlend(ui::BlendMode::Additive);
    dl.addBlob(c, 16.0f * us, ui::setAlpha(th.face.glow, 0.45f));
    dl.setBlend(ui::BlendMode::Alpha);
    dl.addCircleStroke(c, 7.0f * us, a, std::max(1.0f, 1.5f * us));
    dl.addCircleFilled(c, 2.0f * us, a);
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
  // TX.resize: S + a left press on a block resizes it (before its face's controls: S-drag over a knob scales the block);
  // S + double-click gives it its own size again.
  if (drag_ == Drag::None && e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left &&
      !(e.mods & ui::mod::kAlt) && resizeHeld())
    if (const uint32_t node = resizableAt(e.pos); node != kNone) {
      if (tree()) tree()->setFocus(this);
      const Node& n = pp.nodes[node];
      if (e.clickCount >= 2) {
        (void)v.setBlockScale(n.device, 1.0f);
        return true;
      }
      drag_ = Drag::Resize;
      resizeDevice_ = n.device;
      resizeK0_ = n.scale;
      resizeW0_ = n.w;
      resizeH0_ = n.h;
      press_ = last_ = e.pos;
      markDirty();
      return true;
    }
  // TX.node-menu: a cable held after a Ctrl-release: a click wires the next target (Ctrl+click keeps it), a right click
  // or a click on nothing drops it.
  if (sticky_ && e.type == ui::PointerType::Down) {
    if (e.button != ui::PointerButton::Left) {
      sticky_ = false;
      drag_ = Drag::None;
      from_ = {};
      clearLooseCable();
      markDirty();
      return true;
    }
    swallowUp_ = true;
    const bool keep = (e.mods & ui::mod::kCtrl) != 0;
    const doc::Snapshot before = v.context().doc();
    menuFromDrop_ = false;
    finishWire(e.pos);
    sticky_ = keep && v.context().doc() != before && resolveSource();
    drag_ = sticky_ ? Drag::Wire : Drag::None;
    if (!menuFromDrop_) dropLooseCable();
    if (sticky_) updateLoose(e.pos);
    else from_ = {};
    markDirty();
    return true;
  }
  if (swallowUp_ && e.type == ui::PointerType::Up) {
    swallowUp_ = false;
    return true;
  }
  if (drag_ == Drag::None && e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && !(e.mods & ui::mod::kAlt) &&
      backWireHeld() && pressBackWire(e))
    return true;
  if (lanePointer(e)) return true;  // TX.lane-edit: a window body is the arrangement's editing surface
  if (drag_ == Drag::None && facePointer(e)) return true;  // TMM.4: a face's control (its drag, hover, wheel)
  switch (e.type) {
    case ui::PointerType::Wheel: {
      // T5.x-one-circuit: over a lane window the wheel scrolls its time, Ctrl+wheel zooms it (about the pointer).
      const uint32_t lb = v.laneAt(e.pos);
      if (lb == kNone) return CircuitCanvas::onPointer(e);
      if (e.mods & ui::mod::kCtrl) {
        const LaneRect r = laneBody(pp.nodes[pp.bands[lb].source]);
        const float u = (toWorld(e.pos).x - r.x0) / std::max(1.0f, r.x1 - r.x0);
        v.zoomLanes(static_cast<double>(e.wheel.y), u);
      } else {
        const float notches = std::fabs(e.wheel.x) > std::fabs(e.wheel.y) ? e.wheel.x : -e.wheel.y;
        v.scrollLanes(static_cast<double>(notches));
      }
      return true;
    }
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
        if (v.freeRack()) {  // TX.free-rack: empty space is the rack's (Add Free Device, Zoom to Fit)
          (void)v.openRackMenu(e.pos);
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
      // TX.groups: a group's capsule — double-click its title strip to close it, the closed block to open it again; a
      // press on its frame selects the group (an opened capsule is no scene node) and drags or nudges it as a block.
      if (node != kNone && pp.nodes[node].group) {
        const Node& n = pp.nodes[node];
        const doc::DeviceId g = n.device;
        if (e.clickCount >= 2) {
          if (!n.groupOpen) v.setGroupOpen(g, true);
          else if (toWorld(e.pos).y < n.y + metrics::kCapsuleTitle) v.setGroupOpen(g, false);
          return true;
        }
        if (n.groupOpen) {
          clearSelection();
          v.selectGroup(g);
          press_ = last_ = e.pos;
          drag_ = (e.mods & ui::mod::kCtrl) ? Drag::Nudge : Drag::MaybeMoveDevice;
          dragNode_ = node;
          dragBand_ = kNone;
          return true;
        }
      }
      v.selectGroup({});
      if (e.clickCount >= 2) {
        // T5.x-one-circuit: a clip in a lane window opens its editor (the piano roll, an audio clip's Clip View).
        if (const doc::ClipRef ref = v.laneClipAt(e.pos); ref.clip.valid()) {
          v.openClip(ref);
          return true;
        }
        if (node != kNone && pp.nodes[node].kind == NodeKind::Source) return true;  // a window's empty lane: nothing
        if (node != kNone && pp.nodes[node].kind == NodeKind::Device && !pp.nodes[node].missing) {
          const Node& n = pp.nodes[node];
          if (!n.expanded) {
            v.setDeviceExpanded(n.device, true);  // a closed block opens to its atoms
            return true;
          }
          // Opened (T5.x-one-circuit): its title strip closes it; anywhere else inside edits its circuit (the Device
          // level: the circuit editor) — in an opened rack, the device whose box is under the pointer.
          const graph::Point2 w = toWorld(e.pos);
          if (w.y < n.y + 16.0f) {
            v.setDeviceExpanded(n.device, false);
            return true;
          }
          if (n.meta) {  // TMM.4: a meta module's atoms are its generated expansion
            v.context().setStatus("A meta module's atoms are generated from its face: read-only. Its title strip closes it.");
            return true;
          }
          doc::DeviceId target = n.rack ? doc::DeviceId{} : n.device;
          if (n.rack)
            if (const circuit::ChainCircuit* cc = v.rackCircuit(n.device)) {
              const float dx = n.x + metrics::kExpandedPad - cc->scene->x0, dy = n.y + 16.0f + metrics::kExpandedPad - cc->scene->y0;
              const uint32_t b = cc->deviceBoxAt(w.x - dx, w.y - dy);
              if (b != circuit::kNoBox && cc->boxes[b].kind == circuit::ChainBoxKind::Device && !cc->boxes[b].missing) target = cc->boxes[b].device;
            }
          if (target.valid()) (void)v.editDeviceCircuit(target);
          else v.context().setStatus("A rack has no circuit of its own: double-click a device inside it to edit its circuit");
          return true;
        }
        if (node != kNone && pp.nodes[node].kind == NodeKind::Mixer) {
          const doc::TrackId t = pp.bands[pp.nodes[node].band].track;
          v.setMixerExpanded(t, !v.mixerExpanded(t));
          return true;
        }
        if (node == kNone) {
          if (v.embedded()) v.fitAll();  // TX.groups / TX.free-rack: an embedded canvas has no band to open
          else if (const uint32_t b = bandAt(e.pos); b != kNone) v.openBand(pp.bands[b].track);  // T5.x-circuit-chain
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
      // An output pin: a new wire (a device's export or a note output → a param; a mixer's out / send → a Σ).
      if (const PinHit pin = pinAt(e.pos); pin.valid() && !pin.input) {
        const Node& n = pp.nodes[pin.node];
        if (wireSource(pp, n)) {
          drag_ = Drag::Wire;
          from_ = pin;
          wireCursor_ = ui::Cursor::Crosshair;
          updateLoose(e.pos);  // TX.node-menu-fix1: the real cable comes out of the port
          markDirty();
          return true;
        }
      }
      // A band's title strip: nudge the band (TX.free-rack: the rack's band is only the space around its blocks).
      if (node == kNone && !v.freeRack()) {
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
        // TX.node-menu: Shift + drag splices the block into the cable it is released over; a Shift+click adds it to the
        // selection (on release).
        if (!(pp.nodes[node].group && pp.nodes[node].groupOpen)) {
          drag_ = Drag::MaybeSplice;
          dragNode_ = node;
          spliceKeep_ = selection();
          spliceEdge_ = kNone;
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
          // TX.free-rack: a wire from a block of the rack leaves the canvas: the host (the Device view's cable layer)
          // takes the drag over, onto the chain's knobs and inputs.
          if (v.freeRack() && v.onWireOutside && !rect().contains(e.pos) && from_.valid() && from_.node < pp.nodes.size() &&
              pp.nodes[from_.node].kind == NodeKind::Device && from_.port < pp.nodes[from_.node].outputs.size()) {
            doc::DeviceId from = pp.nodes[from_.node].device;
            std::string port = pp.nodes[from_.node].outputs[from_.port].name;
            if (const size_t at = port.rfind('@'); at != std::string::npos) {  // a closed group's (a rack's) member
              from = doc::DeviceId{static_cast<uint32_t>(std::stoul(port.substr(at + 1)))};
              port.resize(at);
            }
            drag_ = Drag::None;
            from_ = {};
            clearLooseCable();  // the host's cable layer draws it from here
            markDirty();
            v.onWireOutside(from, port, e.pos);
            return true;
          }
          wireCursor_ = wireRefusal(e.pos) != nullptr ? ui::Cursor::NotAllowed : ui::Cursor::Crosshair;
          updateLoose(e.pos);
          markDirty();
          break;
        case Drag::MoveDevice:
          // TX.free-rack: dragged out of the rack, the host shows where the block would land (the chain).
          if (v.freeRack() && v.onBlockOutside && dragNode_ < pp.nodes.size()) {
            if (!rect().contains(e.pos)) {
              outside_ = true;
              (void)v.onBlockOutside(pp.nodes[dragNode_].device, e.pos, false);
            } else if (outside_) {
              leaveOutside(e.pos);
            }
          }
          dragCables();  // TX.cables: its cables follow the dragged outline
          markDirty();
          break;
        case Drag::Nudge:
          dragCables();
          markDirty();
          break;
        case Drag::Resize: {
          // The bottom-right corner follows the pointer along the block's diagonal (its top-left stays): the press-time
          // size, so the re-laid-out block under the pointer does not feed back into the drag.
          const float s = deviceScale();
          const float dx = (e.pos.x - press_.x) / s, dy = (e.pos.y - press_.y) / s;
          const float d2 = std::max(1.0f, resizeW0_ * resizeW0_ + resizeH0_ * resizeH0_);
          const float k = resizeK0_ * (1.0f + (dx * resizeW0_ + dy * resizeH0_) / d2);
          (void)v.setBlockScale(resizeDevice_, ProjectPatchView::clampBlockScale(k, (e.mods & ui::mod::kShift) != 0));
          markDirty();
          break;
        }
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
        case Drag::BackWire:  // TX.node-menu
          updateLoose(e.pos);
          markDirty();
          break;
        case Drag::MaybeSplice:
          if (std::max(std::fabs(e.pos.x - press_.x), std::fabs(e.pos.y - press_.y)) >= drag::kClickSlop * us) {
            drag_ = Drag::Splice;
            updateSplice(e.pos);
          }
          break;
        case Drag::Splice: updateSplice(e.pos); break;
        case Drag::Lane:
        case Drag::None: break;
      }
      return true;
    }
    case ui::PointerType::Up: {
      const Drag d = drag_;
      drag_ = Drag::None;
      if (d == Drag::None) return CircuitCanvas::onPointer(e);
      last_ = e.pos;
      clearCableDrag();  // TX.cables: the scene the drop builds places the blocks (their cables swing on from there)
      switch (d) {
        case Drag::Wire: {
          // TX.node-menu: released with Ctrl held, a made connection keeps a new cable from the same source on the pointer.
          rememberSource();
          const doc::Snapshot before = v.context().doc();
          menuFromDrop_ = false;
          finishWire(e.pos);  // a made connection's new cable takes the loose cable's motion over (no pop)
          if ((e.mods & ui::mod::kCtrl) && v.context().doc() != before && resolveSource()) {
            sticky_ = true;
            drag_ = Drag::Wire;
            last_ = e.pos;
            dropLooseCable();
            updateLoose(e.pos);  // a new cable from the same port on the pointer
            markDirty();
            return true;
          }
          if (!menuFromDrop_) dropLooseCable();  // (the node menu's: it hangs there until the menu closes)
          break;
        }
        case Drag::BackWire:
          menuFromDrop_ = false;
          finishBackWire(e.pos);
          if (!menuFromDrop_) dropLooseCable();
          break;
        case Drag::MaybeSplice:
          if (dragNode_ < v.scene().sceneNode.size() && v.scene().sceneNode[dragNode_] != kNone) {  // a Shift+click: add it
            const uint32_t one[1] = {v.scene().sceneNode[dragNode_]};
            select(one, {}, true);
          }
          break;
        case Drag::Splice: endSplice(true, e.pos); break;
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
        case Drag::Resize: break;  // applied while dragging (view state: no gesture to commit)
        case Drag::Lane:
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
  // TX.patching (SPEC §15 2026-11-01 "free patching in the circuit"): any output of a device (its main output too) or a
  // note output goes onto any parameter or input of any device. TX.all-free: a track's own signal (its source's main
  // output) and any main output onto a main input or the track's mixer: the cables the main path is made of.
  if (a.kind == NodeKind::Device || a.kind == NodeKind::Terminal || isTrackSource(a)) {
    const uint32_t to = nodeAt(pos);
    if (to == kNone) return nullptr;  // empty canvas: the drop does nothing
    const Node& t = pp.nodes[to];
    if (!wireEnd(t)) return "Wire: drop it on a device's parameter or input (or a group's Out, a track's mixer)";
    const PinHit h = pinAt(pos);
    const bool onPin = h.valid() && h.input && h.node == to && h.port < t.inputs.size();
    const bool notes = isTrackSource(a) && isNoteSource(pp, a);  // a MIDI track's note source
    // TX.all-free-fix1: another track's mixer takes a block's output or a track's own audio (a send-like route).
    if (notes && t.kind == NodeKind::Mixer && t.band != a.band)
      return "Wire: a track's notes go into blocks (an instrument's notes, a meta module's input); another track's mixer takes audio";
    if (notes && t.kind == NodeKind::Mixer && from_.port != 0)
      return "Wire: a mixer takes audio (a block's output); a note output goes onto a parameter";
    // The notes (output 0) onto a pin that is not a main input, or onto a block's body (it would mean its param menu).
    if (notes && from_.port == 0 && t.kind != NodeKind::Mixer && (onPin ? t.inputs[h.port].param : !onPin))
      return "Wire: the notes go into a main input (an instrument's notes, a meta module's input); a parameter takes a note output (Hz, gate, velocity)";
    // An instrument's notes input takes notes: a MIDI track's source (output 0) — TX.cross-track (SPEC §15 2026-11-03
    // "tracks are not walls"): any track's, which then plays it with its voices — no audio, no other output.
    if (onPin && t.inputs[h.port].name == "notes" && !(notes && from_.port == 0))
      return "Wire: an instrument's notes input takes a MIDI track's notes (from a track's source)";
    return nullptr;
  }
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

bool ProjectCanvas::resizeHeld() const noexcept {
  const ui::UiTree* t = tree();
  if (t == nullptr || !t->keyHeld(kResizeKey)) return false;
  const ui::Widget* f = t->focus();
  return f == nullptr || !f->acceptsTextInput();  // a text field (a face's type-in) takes S as a letter
}

uint32_t ProjectCanvas::resizableAt(Vec2 pos) const {
  const uint32_t node = nodeAt(pos);
  if (node == kNone) return kNone;
  const Node& n = view_.patch().nodes[node];
  return n.kind == NodeKind::Device && !n.expanded && !(n.group && n.groupOpen) ? node : kNone;  // TX.groups: a capsule is its contents' size
}

ui::Cursor ProjectCanvas::cursorAt(Vec2 pos) const {
  if (faceDragging()) return faceCursor(pos);  // TMM.4
  // TX.resize: S held over a block (what a press would do) and the resize drag itself.
  if (drag_ == Drag::Resize || (drag_ == Drag::None && !panning() && resizeHeld() && resizableAt(pos) != kNone))
    return ui::Cursor::ResizeNWSE;
  switch (drag_) {
    case Drag::Wire: return wireCursor_;
    case Drag::MoveDevice: return bandAt(pos) == kNone && !view_.freeRack() ? ui::Cursor::NotAllowed : ui::Cursor::Move;
    case Drag::MaybeMoveDevice:
    case Drag::Nudge: return ui::Cursor::Move;
    case Drag::Volume:
    case Drag::Pan:
    case Drag::Send: return ui::Cursor::ResizeEW;
    case Drag::Resize: return ui::Cursor::ResizeNWSE;
    case Drag::Lane: return laneCursor_;  // TX.lane-edit: the arrangement's gesture cursor
    case Drag::BackWire: return ui::Cursor::Crosshair;  // TX.node-menu
    case Drag::MaybeSplice:
    case Drag::Splice: return ui::Cursor::Move;
    case Drag::None: break;
  }
  if (laneHover_ && !panning() && scene()) return laneCursor_;  // TX.lane-edit: over a window body (Alt: copy / curve)
  if (panning() || altHeld() || !scene()) return CircuitCanvas::cursorAt(pos);
  return hoverCursor_;
}

ui::Cursor ProjectCanvas::pressCursor(Vec2 pos) const {
  // The order of onPointer's left press (Alt and the middle button pan: the canvas's).
  if (const ui::Cursor c = faceCursor(pos); c != ui::Cursor::Inherit) return c;  // TMM.4: a face's control
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
  if (const PinHit pin = pinAt(pos); pin.valid() && !pin.input && wireSource(pp, pp.nodes[pin.node])) return ui::Cursor::Crosshair;
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
  // TX.node-menu: an output's cable released on empty space: the node menu (nodes with an input first); the node arrives
  // wired from that output.
  if ((a.kind == NodeKind::Device || a.kind == NodeKind::Terminal || isTrackSource(a)) && nodeAt(pos) == kNone && !pinAt(pos).valid() &&
      rect().contains(pos)) {
    ProjectPatchView::SpawnLink link;
    link.kind = ProjectPatchView::SpawnLink::Kind::FromOutput;
    link.port = a.outputs[from_.port].name;
    if (isTrackSource(a)) {
      link.notes = fromBand.track;
      if (from_.port == 0) link.port.clear();  // TX.all-free: the track's own signal (kMainPort)
    } else {
      link.device = a.device;
      if (const size_t at = link.port.rfind('@'); at != std::string::npos && (a.rack || a.group)) {
        link.device = doc::DeviceId{static_cast<uint32_t>(std::stoul(link.port.substr(at + 1)))};
        link.port.resize(at);
      }
    }
    menuFromDrop_ = v.openSpawnMenu(pos, link) != nullptr;
    return;
  }
  // Any output of a device or (TMM.10-fix2) a note output: onto an input pin (an audio input, a wired input, a param a
  // wire drives), that port; TX.patching: onto a face's control, that param; onto a device anywhere else, a menu of its
  // params and inputs.
  if (a.kind == NodeKind::Device || a.kind == NodeKind::Terminal || isTrackSource(a)) {
    // TX.all-free: a track source's output 0 is its own signal (kMainPort); its others a MIDI track's note outputs.
    const std::string port = isTrackSource(a) && from_.port == 0 ? std::string() : a.outputs[from_.port].name;
    const bool device = a.kind == NodeKind::Device || a.kind == NodeKind::Terminal;  // TX.groups: In is the group's port
    const doc::DeviceId from = device ? a.device : doc::DeviceId{};
    const doc::TrackId notes = device ? doc::TrackId{} : fromBand.track;
    auto wire = [&](doc::DeviceId to, const std::string& param) {
      (void)(notes.valid() ? v.connectNotes(notes, port, to, param) : v.connect(from, port, to, param));
    };
    // TX.all-free: onto the band's mixer: into its input (the track's signal at the end of its path).
    if (const uint32_t to = nodeAt(pos); to != kNone && pp.nodes[to].kind == NodeKind::Mixer) {
      doc::Wire w;
      w.from = from;
      w.noteSource = notes;
      w.port = port;
      w.mixer = pp.bands[pp.nodes[to].band].track;
      (void)v.connectEnds(std::move(w));
      return;
    }
    const PinHit target = pinAt(pos);
    if (target.valid() && target.input && wireEnd(pp.nodes[target.node])) {
      const Node& t = pp.nodes[target.node];
      if (const Port* q = wireTarget(t, target.port)) {
        std::string param = q->name;
        // TX.all-free: an instrument's `notes` takes a track's source (the notes: TX.cross-track — any MIDI track's, which
        // then plays it with its voices; its own track's is a main link, the head of the path); a rack's input only a
        // main output (a main link). Any other output there is refused, as before.
        const bool notesIn = param == "notes";
        const bool mainOnly = notesIn || (t.rack && !q->param);
        const bool fromSource = isTrackSource(a) && from_.port == 0 && (a.band == t.band || (notesIn && isNoteSource(pp, a)));
        const bool fromMain = fromSource || (!notesIn && a.kind == NodeKind::Device &&
                                             a.outputs[from_.port].name == v.mainPorts(a.device).out);
        if (mainOnly && !fromMain) return;
        if (mainOnly) param.clear();
        doc::DeviceId to = t.device;
        if (const size_t at = param.rfind('@'); t.rack && at != std::string::npos) {  // T4.8: a device inside the rack
          to = doc::DeviceId{static_cast<uint32_t>(std::stoul(param.substr(at + 1)))};
          param.resize(at);
        }
        wire(to, param);
        return;
      }
    }
    // TX.patching: a face's control (the Phase accumulator's frequency or phase, a VCA's knob …): that param;
    // TX.panel-faces: a panel's control (a knob, a box, a toggle, a display's handle): its param.
    if (const FaceParamHit fh = faceParamAt(pos); !fh.param.empty() && fh.node < v.scene().modelNode.size()) {
      const uint32_t mn = v.scene().modelNode[fh.node];
      if (mn < pp.nodes.size() && pp.nodes[mn].kind == NodeKind::Device && (pp.nodes[mn].face != nullptr || pp.nodes[mn].panel) &&
          v.scene().sceneNode[mn] == fh.node) {
        wire(pp.nodes[mn].device, fh.param);
        return;
      }
    }
    const uint32_t to = nodeAt(pos);
    if (to != kNone && pp.nodes[to].kind == NodeKind::Device) (void)v.openParamMenu(from, port, to, pos, notes);
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

void ProjectCanvas::leaveOutside(Vec2 pos) {
  if (!outside_) return;
  outside_ = false;
  if (view_.onBlockOutside) (void)view_.onBlockOutside({}, pos, false);
}

void ProjectCanvas::finishDeviceMove(Vec2 pos) {
  ProjectPatchView& v = view_;
  const ProjectPatch& pp = v.patch();
  if (dragNode_ == kNone) return;
  // TX.free-rack: released outside the rack: the host's (into the chain); inside, the block moves where it is dropped
  // (or joins the opened capsule it is dropped into, or leaves its own).
  if (v.freeRack()) {
    const doc::DeviceId device = pp.nodes[dragNode_].device;
    if (!rect().contains(pos)) {
      outside_ = false;
      if (v.onBlockOutside) (void)v.onBlockOutside(device, pos, true);
      return;
    }
    leaveOutside(pos);
    const Node& moved = pp.nodes[dragNode_];
    const graph::Point2 w = toWorld(pos), start = toWorld(press_);
    uint32_t into = kNone;
    for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
      const Node& c = pp.nodes[i];
      if (i != dragNode_ && c.group && c.groupOpen && w.x >= c.x && w.x <= c.x + c.w && w.y >= c.y && w.y <= c.y + c.h) into = i;
    }
    if (moved.capsule != kNone && into != moved.capsule) {
      (void)v.setDeviceGroup(device, into != kNone ? pp.nodes[into].device : doc::DeviceId{});
      return;
    }
    if (moved.capsule == kNone && into != kNone && !moved.group) {
      (void)v.setDeviceGroup(device, pp.nodes[into].device);
      return;
    }
    v.nudge(device.value, w.x - start.x, w.y - start.y);
    return;
  }
  const uint32_t b = bandAt(pos);
  if (b == kNone) return;
  const Band& band = pp.bands[b];
  const Node& moved = pp.nodes[dragNode_];
  const graph::Point2 w = toWorld(pos);
  if (pp.scoped && moved.group) return;  // TX.groups: a group panel holds the group itself still
  const doc::Track* now = doc::findDevice(*v.context().doc(), moved.device).track;
  const graph::Point2 start = toWorld(press_);
  // TX.groups: inside a capsule a block moves freely (a nudge); dragged out of it, it leaves the group; a block dropped
  // into another opened capsule of its band joins that group (from the main path: it leaves the path, joined there).
  {
    auto insideNode = [&](const Node& c) { return w.x >= c.x && w.x <= c.x + c.w && w.y >= c.y && w.y <= c.y + c.h; };
    uint32_t into = kNone;
    for (uint32_t i = 0; i < pp.nodes.size(); ++i)
      if (i != dragNode_ && pp.nodes[i].group && pp.nodes[i].groupOpen && pp.nodes[i].band == b && insideNode(pp.nodes[i])) into = i;
    if (moved.capsule != kNone) {
      if (into == moved.capsule) v.nudge(moved.device.value, w.x - start.x, w.y - start.y);
      else if (now != nullptr && now->id == band.track) (void)v.setDeviceGroup(moved.device, into != kNone ? pp.nodes[into].device : doc::DeviceId{});
      return;
    }
    if (into != kNone && !moved.group && now != nullptr && now->id == band.track) {
      (void)v.setDeviceGroup(moved.device, pp.nodes[into].device);
      return;
    }
  }
  // TX.all-free (SPEC §15 2026-11-03 "everything is free"): in its own band a block moves where it is dropped (a nudge;
  // its cables are its place in the signal path, nothing is locked into a row); onto another band it moves to that
  // track, off its path there (its old path joined where it was), where it was dropped.
  if (now != nullptr && now->id == band.track) {
    v.nudge(moved.device.value, w.x - start.x, w.y - start.y);
    return;
  }
  (void)v.moveToTrack(moved.device, band.track, w);
}

// TX.lane-edit: a press in a window body goes to the arrangement's automation and editor (lane_edit.cpp: the surface
// bound for the call); its drag keeps the pointer (Drag::Lane) until it ends. Over a body (no drag) the hover shows what a
// press would grab, and laneCursor_ is its cursor. A double-click on a clip opens its editor (the piano roll, Clip View).
bool ProjectCanvas::lanePointer(const ui::PointerEvent& e) {
  ProjectPatchView& v = view_;
  if (drag_ == Drag::Lane) {
    if (e.type == ui::PointerType::Wheel) return false;
    (void)v.lanePointer(e);
    if (e.type == ui::PointerType::Move || e.type == ui::PointerType::Up) laneCursor_ = v.laneCursor(e.pos, e.mods);
    if (!v.laneDragging()) {
      drag_ = Drag::None;
      laneHover_ = v.laneBodyAt(e.pos) != kNone;
      if (laneHover_) v.laneHover(e.pos, e.mods);
    }
    return true;
  }
  switch (e.type) {
    case ui::PointerType::Down: {
      if (drag_ != Drag::None || (e.button != ui::PointerButton::Left && e.button != ui::PointerButton::Right) ||
          v.laneBodyAt(e.pos) == kNone) {
        if (e.button == ui::PointerButton::Left || e.button == ui::PointerButton::Right) laneFocus_ = false;
        return false;
      }
      if (tree()) tree()->setFocus(this);
      laneFocus_ = true;
      clearSelection();  // one selection at a time: the window's (Delete, Ctrl+D … act on the clips)
      v.selectGroup({});
      if (e.button == ui::PointerButton::Left && e.clickCount >= 2 && (e.mods & (ui::mod::kCtrl | ui::mod::kShift)) == 0)
        if (const doc::ClipRef ref = v.laneClipAt(e.pos); ref.clip.valid()) {
          v.openClip(ref);
          return true;
        }
      laneHover_ = false;
      v.clearLaneHover();
      (void)v.lanePointer(e);
      if (v.laneDragging()) {
        drag_ = Drag::Lane;
        laneCursor_ = v.laneCursor(e.pos, e.mods);
      }
      return true;
    }
    case ui::PointerType::Move: {
      if (drag_ != Drag::None) return false;
      if (!panning() && v.laneBodyAt(e.pos) != kNone) {
        v.laneHover(e.pos, e.mods);
        laneCursor_ = v.laneCursor(e.pos, e.mods);
        laneHover_ = true;
      } else if (laneHover_) {
        v.clearLaneHover();
        laneHover_ = false;
      }
      return false;  // the canvas's own hover (readouts, info) goes on
    }
    case ui::PointerType::Leave:
      if (laneHover_) {
        v.clearLaneHover();
        laneHover_ = false;
      }
      return false;
    default: return false;
  }
}

bool ProjectCanvas::onKey(const ui::KeyEvent& e) {
  if (faceKey(e)) return true;  // TMM.7: the focused face first (a keyframe's Delete before the selection's)
  if (!e.down) return false;
  const ui::Mods mods = e.mods & ui::mod::kChordMask;
  if (e.key == ui::key::kEscape && drag_ == Drag::Lane) {  // TX.lane-edit: the window's drag ends without an edit
    view_.laneAbort();
    drag_ = Drag::None;
    markDirty();
    return true;
  }
  if (e.key == ui::key::kEscape && (drag_ != Drag::None || faceDragging())) {  // TMM.4: a face's drag too
    onInteractionCancelled();
    return true;
  }
  // TX.lane-edit: after a press in a window, the arrangement's edit chords act there (Ctrl+Shift+M, Delete, Ctrl+D …).
  if (laneFocus_ && view_.laneKey(e)) return true;
  // TX.node-menu: Esc drops a held cable; Tab opens the node menu at the pointer (over a cable: insert there); Z over a
  // cable inserts a VCA at 1; Ctrl+Delete dissolves the selected block.
  if (e.key == ui::key::kEscape && sticky_) {
    sticky_ = false;
    drag_ = Drag::None;
    from_ = {};
    clearLooseCable();
    markDirty();
    return true;
  }
  if (drag_ == Drag::None && mods == ui::mod::kNone && (e.key == ui::key::kTab || e.key == 'z')) {
    Vec2 at = pointer_;
    if (!rect().contains(at)) at = {static_cast<float>(rect().x + rect().w / 2), static_cast<float>(rect().y + rect().h / 2)};
    uint32_t edge = edgeAt(at);
    // TX.node-menu-fix1: over a cable nothing can be inserted into (notes, routes, sends), Tab adds a node as on empty
    // canvas (it did nothing but a status line).
    if (e.key == ui::key::kTab && edge != kNone && !cuttableEdge(edge)) edge = kNone;
    if (e.key == 'z') {
      if (edge == kNone) return CircuitCanvas::onKey(e);
      (void)view_.insertVca(edge);
      return true;
    }
    if (edge != kNone) (void)view_.openInsertMenu(edge, at);
    else (void)view_.openSpawnMenu(at);
    return true;
  }
  if (mods == ui::mod::kCtrl && (e.key == ui::key::kDelete || e.key == ui::key::kBackspace)) {
    const std::vector<doc::DeviceId> d = view_.selectedDevices();
    if (d.size() != 1) {
      view_.context().setStatus("Dissolve: select one block (Ctrl+Delete)");
      return !view_.embedded();
    }
    return view_.dissolve(d.front()) || !view_.embedded();
  }
  // TMM.13-fix2 (MM review M5): an embedded canvas (a group's panel, the free rack) reports these handled only when
  // they did something, so the Device view around it still gets Delete, Ctrl+G and Ctrl+Shift+G. The main view keeps
  // them (a Delete there must never fall through to the arrangement's selection behind it).
  const bool swallow = !view_.embedded();
  if (mods == ui::mod::kNone && (e.key == ui::key::kDelete || e.key == ui::key::kBackspace))
    return view_.deleteSelection() || swallow;
  // TX.groups: Ctrl+G groups the selection, Ctrl+Shift+G ungroups (before the keymap: the Device view's Ctrl+G makes a
  // rack there).
  if (e.key == 'g' && mods == ui::mod::kCtrl) return view_.groupSelection() || swallow;
  if (e.key == 'g' && mods == (ui::mod::kCtrl | ui::mod::kShift)) return view_.ungroupSelection() || swallow;
  if (mods == ui::mod::kNone && e.key == '0') {
    const std::vector<doc::DeviceId> d = view_.selectedDevices();
    return (!d.empty() && view_.toggleDevicesActive(d)) || swallow;
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

// TX.cables: the dragged block (an opened capsule: its contents; a band's title: the band's blocks) carries its cables'
// ends with the preview outline.
void ProjectCanvas::dragCables() {
  const ProjectPatch& pp = view_.patch();
  const ProjectScene& ps = view_.scene();
  std::vector<uint32_t> nodes;
  for (uint32_t v = 0; v < ps.modelNode.size(); ++v) {
    const uint32_t m = ps.modelNode[v];
    if (m >= pp.nodes.size()) continue;
    const Node& n = pp.nodes[m];
    if (dragNode_ != kNone ? (m == dragNode_ || n.capsule == dragNode_) : (dragBand_ != kNone && n.band == dragBand_)) nodes.push_back(v);
  }
  const float s = std::max(deviceScale(), 1e-6f);
  setCableDrag(nodes, {(last_.x - press_.x) / s, (last_.y - press_.y) / s});
}

void ProjectCanvas::onCaptureLost() {
  clearCableDrag();  // TX.cables
  if (drag_ == Drag::Splice) endSplice(false, last_);  // TX.node-menu
  if (sticky_) return;  // TX.node-menu: a held cable has no capture (no button is down)
  if (drag_ == Drag::Wire || drag_ == Drag::BackWire) clearLooseCable();  // TX.node-menu-fix1
  if (drag_ == Drag::Volume || drag_ == Drag::Pan || drag_ == Drag::Send) view_.endValueGesture();
  if (drag_ == Drag::Lane) view_.laneAbort();  // TX.lane-edit (as the arrangement's lanes: no partial edit)
  leaveOutside(last_);  // TX.free-rack
  if (drag_ != Drag::None) markDirty();
  drag_ = Drag::None;
  dragNode_ = dragBand_ = kNone;
  CircuitCanvas::onCaptureLost();
}

void ProjectCanvas::onInteractionCancelled() {
  clearCableDrag();  // TX.cables: the cables swing back with the outline
  if (drag_ == Drag::Wire || drag_ == Drag::BackWire || sticky_) clearLooseCable();  // TX.node-menu-fix1
  if (drag_ == Drag::Splice) endSplice(false, last_);  // TX.node-menu
  sticky_ = false;
  if (drag_ == Drag::Volume || drag_ == Drag::Pan || drag_ == Drag::Send) view_.cancelValueGesture();
  if (drag_ == Drag::Lane) view_.laneAbort();  // TX.lane-edit
  leaveOutside(last_);  // TX.free-rack
  if (drag_ == Drag::Resize) {  // TX.resize: Esc / focus lost: back to the size at the press
    drag_ = Drag::None;
    (void)view_.setBlockScale(resizeDevice_, resizeK0_);
  }
  if (drag_ != Drag::None) markDirty();
  drag_ = Drag::None;
  dragNode_ = dragBand_ = kNone;
  from_ = {};
  CircuitCanvas::onInteractionCancelled();
}

// ---- TX.node-menu ----------------------------------------------------------------------------------------------------

bool ProjectCanvas::backWireHeld() const noexcept {
  const ui::UiTree* t = tree();
  if (t == nullptr || !t->keyHeld(kBackWireKey)) return false;
  const ui::Widget* f = t->focus();
  return f == nullptr || !f->acceptsTextInput();
}

uint32_t ProjectCanvas::edgeAt(Vec2 device) const {
  if (!scene() || !rect().contains(device)) return kNone;
  const circuit::CanvasHit h = hitTest(device);
  const ProjectScene& ps = view_.scene();
  if (h.kind != circuit::CanvasHit::Kind::Wire || h.index >= ps.modelEdge.size()) return kNone;
  return ps.modelEdge[h.index];
}

ProjectCanvas::NodeKey ProjectCanvas::keyOf(uint32_t node) const noexcept {
  const ProjectPatch& pp = view_.patch();
  if (node >= pp.nodes.size()) return {};
  const Node& n = pp.nodes[node];
  return {n.kind, n.device, n.band < pp.bands.size() ? pp.bands[n.band].track : doc::TrackId{}, true};
}

uint32_t ProjectCanvas::findKey(const NodeKey& k) const noexcept {
  if (!k.valid) return kNone;
  const ProjectPatch& pp = view_.patch();
  for (uint32_t i = 0; i < pp.nodes.size(); ++i) {  // rare (a rebuild during a gesture): a linear search
    const Node& n = pp.nodes[i];
    if (n.kind == k.kind && n.device == k.device && n.band < pp.bands.size() && pp.bands[n.band].track == k.track) return i;
  }
  return kNone;
}

void ProjectCanvas::beforeRebuild() {
  const ProjectPatch& pp = view_.patch();
  keepNode_ = keyOf(dragNode_);
  keepBand_ = dragBand_ < pp.bands.size() ? pp.bands[dragBand_].track : doc::TrackId{};
  keepFrom_ = from_.valid() ? keyOf(from_.node) : NodeKey{};
  keepFromPort_.clear();
  keepFromInput_ = from_.input;
  if (keepFrom_.valid && from_.node < pp.nodes.size()) {
    const Node& n = pp.nodes[from_.node];
    const std::vector<Port>& ports = from_.input ? n.inputs : n.outputs;
    if (from_.port < ports.size()) keepFromPort_ = ports[from_.port].name;
  }
}

void ProjectCanvas::afterRebuild() {
  const ProjectPatch& pp = view_.patch();
  bool lost = false;
  if (keepNode_.valid) {
    dragNode_ = findKey(keepNode_);
    lost = lost || dragNode_ == kNone;
  } else {
    dragNode_ = kNone;
  }
  if (keepBand_.valid()) {
    dragBand_ = pp.bandOf(keepBand_);
    lost = lost || dragBand_ == kNone;
  } else {
    dragBand_ = kNone;
  }
  if (keepFrom_.valid) {
    from_ = {};
    if (const uint32_t n = findKey(keepFrom_); n != kNone) {
      const int32_t k = keepFromInput_ ? pp.nodes[n].input(keepFromPort_) : pp.nodes[n].output(keepFromPort_);
      if (k >= 0) from_ = {n, static_cast<uint32_t>(k), keepFromInput_};
    }
    lost = lost || (!from_.valid() && (drag_ == Drag::Wire || sticky_));
  }
  // A splice's cable: found again on the next move (edge indices are the model's).
  if (drag_ == Drag::Splice || drag_ == Drag::MaybeSplice) {
    spliceEdge_ = kNone;
    spliceKeep_ = selection();
  }
  keepNode_ = keepFrom_ = {};
  keepBand_ = {};
  const bool indexed = drag_ == Drag::Wire || drag_ == Drag::MaybeMoveDevice || drag_ == Drag::MoveDevice || drag_ == Drag::Nudge ||
                       drag_ == Drag::Volume || drag_ == Drag::Pan || drag_ == Drag::Send || drag_ == Drag::MaybeSplice ||
                       drag_ == Drag::Splice;
  if (lost && (indexed || sticky_)) {
    // What the gesture held went away (an undo, another edit): it ends here, without an edit (its release does nothing).
    if (drag_ == Drag::Volume || drag_ == Drag::Pan || drag_ == Drag::Send) view_.cancelValueGesture();
    clearCableDrag();
    if (drag_ == Drag::Wire || drag_ == Drag::BackWire || sticky_) clearLooseCable();
    drag_ = Drag::None;
    sticky_ = false;
    dragNode_ = dragBand_ = kNone;
    from_ = {};
    spliceEdge_ = kNone;
    markDirty();
  }
}

bool ProjectCanvas::cuttableEdge(uint32_t edge) const noexcept {
  const ProjectPatch& pp = view_.patch();
  if (edge >= pp.edges.size()) return false;
  const EdgeKind k = pp.edges[edge].kind;
  return k == EdgeKind::Chain || k == EdgeKind::Modulation;
}

void ProjectCanvas::rememberSource() {
  const ProjectPatch& pp = view_.patch();
  fromDevice_ = {};
  fromNotes_ = {};
  fromPort_.clear();
  if (!from_.valid() || from_.node >= pp.nodes.size()) return;
  const Node& n = pp.nodes[from_.node];
  if (from_.port < n.outputs.size()) fromPort_ = n.outputs[from_.port].name;
  if (isTrackSource(n)) fromNotes_ = pp.bands[n.band].track;
  else fromDevice_ = n.device;
}

bool ProjectCanvas::resolveSource() {
  view_.sync();
  const ProjectPatch& pp = view_.patch();
  from_ = {};
  uint32_t node = kNone;
  if (fromNotes_.valid()) {
    const uint32_t b = pp.bandOf(fromNotes_);
    if (b != kNone) node = pp.bands[b].source;
  } else if (fromDevice_.valid()) {
    node = pp.nodeOf(fromDevice_);
  }
  if (node == kNone) return false;
  const int32_t k = pp.nodes[node].output(fromPort_);
  if (k < 0) return false;
  from_ = {node, static_cast<uint32_t>(k), false};
  return true;
}

bool ProjectCanvas::pressBackWire(const ui::PointerEvent& e) {
  const ProjectPatch& pp = view_.patch();
  backDevice_ = {};
  backParam_.clear();
  if (const PinHit pin = pinAt(e.pos); pin.valid() && pin.input && pin.node < pp.nodes.size()) {
    const Node& t = pp.nodes[pin.node];
    if (const Port* q = wireTarget(t, pin.port)) {
      backParam_ = q->name;
      backDevice_ = t.device;
      if (const size_t at = backParam_.rfind('@'); at != std::string::npos && (t.rack || t.group)) {
        backDevice_ = doc::DeviceId{static_cast<uint32_t>(std::stoul(backParam_.substr(at + 1)))};
        backParam_.resize(at);
      }
      backWorld_ = inputEntry(t, pin.port);
      backStart_ = toDevice(backWorld_);
    }
  }
  if (!backDevice_.valid())
    if (const FaceParamHit fh = faceParamAt(e.pos); !fh.param.empty() && fh.node < view_.scene().modelNode.size()) {
      const uint32_t mn = view_.scene().modelNode[fh.node];
      if (mn < pp.nodes.size() && pp.nodes[mn].kind == NodeKind::Device) {
        backDevice_ = pp.nodes[mn].device;
        backParam_ = fh.param;
        backStart_ = e.pos;
        backWorld_ = faceControlPoint(fh.node, fh.param, toWorld(e.pos)).value_or(toWorld(e.pos));
      }
    }
  if (!backDevice_.valid()) return false;
  if (tree()) tree()->setFocus(this);
  drag_ = Drag::BackWire;
  press_ = last_ = e.pos;
  updateLoose(e.pos);  // TX.node-menu-fix1: the real cable comes out of the param
  markDirty();
  return true;
}

void ProjectCanvas::updateLoose(Vec2 pos) {
  const ProjectPatch& pp = view_.patch();
  graph::Point2 port{}, to = toWorld(pos);
  circuit::WireStyle style;
  bool snapped = false;
  if (drag_ == Drag::Wire && from_.valid() && from_.node < pp.nodes.size() && !from_.input) {
    const Node& n = pp.nodes[from_.node];
    port = {n.x + n.w, n.y + pinY(n, from_.port)};  // where its cable will start (the model's outPin)
    style.notes = isNoteSource(pp, n);
    if (wireRefusal(pos) == nullptr && (n.kind == NodeKind::Device || n.kind == NodeKind::Terminal || style.notes)) {
      // Snapped onto what a release here connects: an input pin (that port), a face's / panel's control (its param).
      const PinHit t = pinAt(pos);
      if (t.valid() && t.input && t.node < pp.nodes.size() && t.node != from_.node && wireEnd(pp.nodes[t.node])) {
        if (const Port* q = wireTarget(pp.nodes[t.node], t.port)) {
          to = inputEntry(pp.nodes[t.node], t.port);
          style.modulation = q->param;
          snapped = true;
        }
      }
      if (!snapped)
        if (const FaceParamHit fh = faceParamAt(pos); !fh.param.empty() && fh.node < view_.scene().modelNode.size() &&
                                                      view_.scene().modelNode[fh.node] != from_.node)
          if (const std::optional<graph::Point2> c = faceControlPoint(fh.node, fh.param, port)) {
            to = *c;
            style.modulation = true;
            snapped = true;
          }
    }
  } else if (drag_ == Drag::BackWire && backDevice_.valid()) {
    port = backWorld_;
    style.modulation = true;
    // Snapped onto the output a release here would drive the param from: an output pin, or a block's main output.
    if (const PinHit pin = pinAt(pos); pin.valid() && !pin.input && pin.node < pp.nodes.size()) {
      const Node& n = pp.nodes[pin.node];
      if ((n.kind == NodeKind::Device || n.kind == NodeKind::Terminal || isNoteSource(pp, n)) && n.device != backDevice_ &&
          !(isNoteSource(pp, n) && pin.port == 0)) {
        to = {n.x + n.w, n.y + pinY(n, pin.port)};
        style.notes = isNoteSource(pp, n);
        snapped = true;
      }
    } else if (const uint32_t b = nodeAt(pos); b != kNone && pp.nodes[b].kind == NodeKind::Device && pp.nodes[b].device != backDevice_) {
      const Node& n = pp.nodes[b];
      const circuit::MainPorts mp = view_.mainPorts(n.device);
      if (const int32_t k = mp.out.empty() ? -1 : n.output(mp.out); k >= 0) {
        to = {n.x + n.w, n.y + pinY(n, static_cast<size_t>(k))};
        snapped = true;
      }
    }
  } else {
    return;
  }
  setLooseCable(port, to, style, snapped);
}

void ProjectCanvas::finishBackWire(Vec2 pos) {
  ProjectPatchView& v = view_;
  const ProjectPatch& pp = v.patch();
  if (!backDevice_.valid()) return;
  // Onto an output pin (a device's, a group's In, a note output): that output drives the param.
  if (const PinHit pin = pinAt(pos); pin.valid() && !pin.input && pin.node < pp.nodes.size()) {
    const Node& n = pp.nodes[pin.node];
    std::string port = pin.port < n.outputs.size() ? n.outputs[pin.port].name : std::string();
    if (isNoteSource(pp, n)) {
      if (pin.port == 0) v.context().setStatus("Wire: drag from a note output (Hz, gate, velocity), not the notes");
      else (void)v.connectNotes(pp.bands[n.band].track, port, backDevice_, backParam_);
      return;
    }
    if (n.kind == NodeKind::Device || n.kind == NodeKind::Terminal) {
      doc::DeviceId from = n.device;
      if (const size_t at = port.rfind('@'); at != std::string::npos && (n.rack || n.group)) {
        from = doc::DeviceId{static_cast<uint32_t>(std::stoul(port.substr(at + 1)))};
        port.resize(at);
      }
      (void)v.connect(from, port, backDevice_, backParam_);
      return;
    }
    v.context().setStatus("Wire: drop a backwards cable on a device's output");
    return;
  }
  // Onto a block: its main output.
  if (const uint32_t n = nodeAt(pos); n != kNone) {
    const Node& nd = pp.nodes[n];
    if (nd.kind != NodeKind::Device || nd.device == backDevice_) return;
    const circuit::MainPorts mp = v.mainPorts(nd.device);
    if (mp.out.empty()) {
      v.context().setStatus("Wire: " + nd.title + " has no output");
      return;
    }
    (void)v.connect(nd.device, mp.out, backDevice_, backParam_);
    return;
  }
  if (!rect().contains(pos)) return;
  // Empty space: the menu offers sources first; the new node's main output drives the param.
  ProjectPatchView::SpawnLink link;
  link.kind = ProjectPatchView::SpawnLink::Kind::IntoParam;
  link.device = backDevice_;
  link.port = backParam_;
  menuFromDrop_ = v.openSpawnMenu(pos, link) != nullptr;
}

void ProjectCanvas::updateSplice(Vec2 pos) {
  last_ = pos;
  const ProjectPatch& pp = view_.patch();
  uint32_t edge = edgeAt(pos);
  if (edge != kNone && edge < pp.edges.size()) {
    const Edge& e = pp.edges[edge];
    const bool own = e.from == dragNode_ || e.to == dragNode_;
    const bool cuttable = e.kind == EdgeKind::Chain || e.kind == EdgeKind::Modulation;
    if (own || !cuttable) edge = kNone;
  }
  if (edge != spliceEdge_) {
    spliceEdge_ = edge;
    // The cable it would go into is shown selected (the canvas's own highlight: straight or hanging, as drawn).
    if (edge != kNone) {
      const ProjectScene& ps = view_.scene();
      for (uint32_t w = 0; w < ps.modelEdge.size(); ++w)
        if (ps.modelEdge[w] == edge) {
          const uint32_t one[1] = {w};
          select({}, one);
          break;
        }
    } else {
      select(spliceKeep_.nodes, spliceKeep_.wires);
    }
  }
  markDirty();
}

void ProjectCanvas::endSplice(bool apply, Vec2 pos) {
  const uint32_t edge = spliceEdge_;
  spliceEdge_ = kNone;
  const ProjectPatch& pp = view_.patch();
  const doc::DeviceId device = dragNode_ < pp.nodes.size() ? pp.nodes[dragNode_].device : doc::DeviceId{};
  select(spliceKeep_.nodes, spliceKeep_.wires);
  (void)pos;
  if (apply && edge != kNone && device.valid()) (void)view_.spliceIntoEdge(device, edge);
  markDirty();
}

ui::InfoText ProjectCanvas::info() const {
  if (drag_ == Drag::Resize || (drag_ == Drag::None && resizeHeld() && resizableAt(pointer_) != kNone))  // TX.resize
    return {"Resize block",
            "S-drag scales the block (0.5x to 3x; Shift: steps of 25 %); everything on it in proportion, the blocks after it "
            "make room. S + double-click (or Reset Size in its menu) gives it its own size. Esc cancels the drag."};
  if (ui::InfoText t; faceInfo(t)) return t;  // TMM.4
  const ProjectPatchView::Readout& ro = view_.readout();
  if (ro.edge != kNone) {
    // TX.node-menu-fix1: what the keys do on this cable (the hover halo shows which one).
    if (!cuttableEdge(ro.edge)) return {ro.title, ro.value};
    infoTitle_ = ro.value + "  ·  Z inserts a VCA at x1, Tab inserts a node, a block Shift-dragged onto it is spliced in.";
    return {ro.title, infoTitle_};
  }
  // TMM.10-fix2: a MIDI track's lane window is its note source (its Hz piano roll), not a subcircuit.
  if (const circuit::CanvasHit& h = hovered(); (h.kind == circuit::CanvasHit::Kind::Node || h.kind == circuit::CanvasHit::Kind::Pin) && scene()) {
    const circuit::CircuitScene& sc = *scene();
    const uint32_t v = h.kind == circuit::CanvasHit::Kind::Pin ? (h.index < sc.pins.size() ? sc.pins[h.index].node : kNone) : h.index;
    const ProjectScene& ps = view_.scene();
    const ProjectPatch& pp = view_.patch();
    if (v < ps.modelNode.size() && ps.modelNode[v] < pp.nodes.size() && isNoteSource(pp, pp.nodes[ps.modelNode[v]])) {
      const Node& n = pp.nodes[ps.modelNode[v]];
      infoTitle_ = pp.bands[n.band].name + " notes";
      if (h.kind == circuit::CanvasHit::Kind::Pin && !sc.pins[h.index].input && sc.pins[h.index].port > 0 &&
          sc.pins[h.index].port < n.outputs.size())
        if (const doc::NoteOutput* o = doc::findNoteOutput(n.outputs[sc.pins[h.index].port].name))
          infoTitle_ += ": " + std::string(o->label);
      return {infoTitle_,
              "The track's note source: its piano roll, in Hz. Its notes play the chain; its note outputs (Hz, gate, velocity) "
              "are per voice: drag one onto a device to wire it into a parameter (per voice into a device that runs per "
              "voice, through the wire's reducer into any other)."};
    }
  }
  if (hovered().kind != circuit::CanvasHit::Kind::None) return CircuitCanvas::info();  // the node, pin or wire under the pointer
  return {"Project Patch",
          "The whole project as one circuit. Drag an export, or a note output of a track's Notes (Hz, gate, velocity), onto a "
          "device to wire it into a parameter, a mixer's output onto a group, return "
          "or the master to route it; double-click a band to open it (its devices opened to their atoms), a device to open it "
          "alone, inside an opened device to edit its circuit. Over a lane window the wheel scrolls time, Ctrl+wheel zooms it; "
          "double-click a clip to edit it; a window edits its track as the arrangement does (drag: a time selection, "
          "Ctrl+Shift+M: a MIDI clip there, a clip's title moves it, Alt copies, its edges trim; A: automation). "
          "Ctrl+G groups the selected blocks into a capsule (Ctrl+Shift+G ungroups). "
          "Ctrl+Alt+P returns to the arrangement."};
}

}  // namespace pq::views::project_patch
