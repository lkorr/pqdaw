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
// Whether a press on output pin `pin` starts a wire: a device's (TX.patching: any output, its main one too), a mixer's
// (route, sends), a note source's.
bool wireSource(const ProjectPatch& pp, const Node& n) noexcept {
  return n.kind == NodeKind::Device || n.kind == NodeKind::Mixer || isNoteSource(pp, n) ||
         (n.kind == NodeKind::Terminal && !n.outputs.empty());  // TX.groups: a group's In terminal
}
// TX.groups: a node a wire may end on (a device, or a group's Out terminal).
bool wireEnd(const Node& n) noexcept { return n.kind == NodeKind::Device || (n.kind == NodeKind::Terminal && !n.inputs.empty()); }

// TX.patching: the port of device node `n`'s input pin `i` a wire dropped on it targets, or null: an instrument's
// `notes` (the chain's note wire) and a rack's own input (its chain's) are not wire targets.
const Port* wireTarget(const Node& n, uint32_t i) noexcept {
  if (!wireEnd(n) || i >= n.inputs.size()) return nullptr;
  const Port& q = n.inputs[i];
  if (q.name == "notes") return nullptr;
  if (n.rack && !q.param) return nullptr;
  return &q;
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
    const float rad = std::min(std::round(10.0f * s), (r.y1 - r.y0) * 0.5f);
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
          paint::text(ctx, font, {box.x + adv + gap, box.y, box.w - adv - gap, box.h}, count, th.circuit.pinText, ui::TextAlign::Left);
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
    for (const Node::MiniBox& b : n.mini) {
      const Vec2 a = at(b.x0, b.y0), c = at(b.x1, b.y1);
      const Rect br{a.x, a.y, std::max(a.x + 2.0f, c.x), std::max(a.y + 2.0f, c.y)};
      dl.addRectFilled(br, b.terminal ? ui::mulAlpha(th.circuit.wireAudio, 0.5f) : th.circuit.symbolFill);
      dl.addRectStroke(br, ui::mulAlpha(th.circuit.symbolStroke, 0.8f), lw);
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
        const Rect box{std::round(cx - w * 0.5f), y0, std::round(cx - w * 0.5f) + w, y0 + lh};
        bool free = true;
        for (const Rect& q : placed) free = free && (box.x1 <= q.x0 || q.x1 <= box.x0 || box.y1 <= q.y0 || q.y1 <= box.y0);
        if (!free) continue;
        placed.push_back(box);
        dl.addRoundedRectFilled(box, 2.0f * us, ui::withAlpha(th.surface.sunken, 225));
        ui::TextSystem::drawRun(dl, run, {box.x0 + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(box.y0), static_cast<int32_t>(lh), static_cast<uint32_t>(px)))},
                                th.circuit.symbolText);
      }
    }
  }
  {
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
          if (const uint32_t b = bandAt(e.pos); b != kNone) v.openBand(pp.bands[b].track);  // T5.x-circuit-chain
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
        case Drag::Resize: break;  // applied while dragging (view state: no gesture to commit)
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
  // note output goes onto any parameter or input of any device; TMM.10-fix2: `notes` is the chain's own note wire.
  if (a.kind == NodeKind::Device || a.kind == NodeKind::Terminal || isNoteSource(pp, a)) {
    if (a.kind == NodeKind::Source && from_.port == 0)
      return "Wire: drag a note output (Hz, gate, velocity: the pins under the notes) onto a parameter or an input";
    const uint32_t to = nodeAt(pos);
    if (to == kNone) return nullptr;  // empty canvas: the drop does nothing
    if (!wireEnd(pp.nodes[to])) return "Wire: drop it on a device's parameter or input (or a group's Out)";
    const PinHit t = pinAt(pos);
    if (t.valid() && t.input && t.node == to && wireTarget(pp.nodes[to], t.port) == nullptr)
      return pp.nodes[to].rack ? "Wire: a rack's input is its chain's; drop it on a parameter or a device inside the rack"
                               : "Wire: the notes input is the chain's own note wire; drop it on a parameter or an input";
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
    case Drag::MoveDevice: return bandAt(pos) == kNone ? ui::Cursor::NotAllowed : ui::Cursor::Move;
    case Drag::MaybeMoveDevice:
    case Drag::Nudge: return ui::Cursor::Move;
    case Drag::Volume:
    case Drag::Pan:
    case Drag::Send: return ui::Cursor::ResizeEW;
    case Drag::Resize: return ui::Cursor::ResizeNWSE;
    case Drag::None: break;
  }
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
  // Any output of a device or (TMM.10-fix2) a note output: onto an input pin (an audio input, a wired input, a param a
  // wire drives), that port; TX.patching: onto a face's control, that param; onto a device anywhere else, a menu of its
  // params and inputs.
  if (a.kind == NodeKind::Device || a.kind == NodeKind::Terminal || isNoteSource(pp, a)) {
    const std::string port = a.outputs[from_.port].name;
    const bool device = a.kind == NodeKind::Device || a.kind == NodeKind::Terminal;  // TX.groups: In is the group's port
    const doc::DeviceId from = device ? a.device : doc::DeviceId{};
    const doc::TrackId notes = device ? doc::TrackId{} : fromBand.track;
    auto wire = [&](doc::DeviceId to, const std::string& param) {
      (void)(notes.valid() ? v.connectNotes(notes, port, to, param) : v.connect(from, port, to, param));
    };
    const PinHit target = pinAt(pos);
    if (target.valid() && target.input && wireEnd(pp.nodes[target.node])) {
      const Node& t = pp.nodes[target.node];
      if (const Port* q = wireTarget(t, target.port)) {
        std::string param = q->name;
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
  // TX.groups: inside a capsule a block moves freely (a nudge); dragged out of it, it leaves the group; a block dropped
  // into another opened capsule of its band joins that group (from the series: it leaves the series).
  {
    auto insideNode = [&](const Node& c) { return w.x >= c.x && w.x <= c.x + c.w && w.y >= c.y && w.y <= c.y + c.h; };
    uint32_t into = kNone;
    for (uint32_t i = 0; i < pp.nodes.size(); ++i)
      if (i != dragNode_ && pp.nodes[i].group && pp.nodes[i].groupOpen && pp.nodes[i].band == b && insideNode(pp.nodes[i])) into = i;
    const graph::Point2 start = toWorld(press_);
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
  // TX.patching (SPEC §15 2026-11-01 "free patching in the circuit"): in its own band, a block of the series dropped
  // below the chain's row leaves the series (free: it goes to the band's free row and keeps its wires); a free block
  // moves where it is dropped (a nudge), or back into the series when dropped on the chain's row.
  if (now != nullptr && now->id == band.track) {
    float chainBottom = band.y + metrics::kTitle + metrics::kPinTop;
    for (const uint32_t n : {band.source, band.mixer})
      if (n != kNone) chainBottom = std::max(chainBottom, pp.nodes[n].y + pp.nodes[n].h);
    for (const uint32_t d : band.devices)
      if (!pp.nodes[d].free) chainBottom = std::max(chainBottom, pp.nodes[d].y + pp.nodes[d].h);
    const doc::DeviceId one[1] = {moved.device};
    if (moved.free && w.y > chainBottom) {
      const graph::Point2 start = toWorld(press_);
      v.nudge(moved.device.value, w.x - start.x, w.y - start.y);
      return;
    }
    if (moved.free) {
      (void)v.putIntoChain(moved.device, index);
      return;
    }
    if (w.y > chainBottom + metrics::kFreeRowGap * 0.5f) {
      (void)v.setDevicesFree(one, true);
      return;
    }
    const int32_t at = now->deviceIndex(moved.device);
    if (at >= 0 && static_cast<uint32_t>(at) == index) return;  // dropped where it was
  }
  (void)v.moveDevice(moved.device, band.track, index);
}

bool ProjectCanvas::onKey(const ui::KeyEvent& e) {
  if (faceKey(e)) return true;  // TMM.7: the focused face first (a keyframe's Delete before the selection's)
  if (!e.down) return false;
  const ui::Mods mods = e.mods & ui::mod::kChordMask;
  if (e.key == ui::key::kEscape && (drag_ != Drag::None || faceDragging())) {  // TMM.4: a face's drag too
    onInteractionCancelled();
    return true;
  }
  if (mods == ui::mod::kNone && (e.key == ui::key::kDelete || e.key == ui::key::kBackspace)) {
    (void)view_.deleteSelection();
    return true;
  }
  // TX.groups: Ctrl+G groups the selection, Ctrl+Shift+G ungroups (before the keymap: the Device view's Ctrl+G makes a
  // rack there).
  if (e.key == 'g' && mods == ui::mod::kCtrl) {
    (void)view_.groupSelection();
    return true;
  }
  if (e.key == 'g' && mods == (ui::mod::kCtrl | ui::mod::kShift)) {
    (void)view_.ungroupSelection();
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

ui::InfoText ProjectCanvas::info() const {
  if (drag_ == Drag::Resize || (drag_ == Drag::None && resizeHeld() && resizableAt(pointer_) != kNone))  // TX.resize
    return {"Resize block",
            "S-drag scales the block (0.5x to 3x; Shift: steps of 25 %); everything on it in proportion, the blocks after it "
            "make room. S + double-click (or Reset Size in its menu) gives it its own size. Esc cancels the drag."};
  if (ui::InfoText t; faceInfo(t)) return t;  // TMM.4
  const ProjectPatchView::Readout& ro = view_.readout();
  if (ro.edge != kNone) return {ro.title, ro.value};
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
          "double-click a clip to edit it. Ctrl+Alt+P returns to the arrangement."};
}

}  // namespace pq::views::project_patch
