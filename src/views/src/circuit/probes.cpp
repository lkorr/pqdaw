// Live probes and cost meters in the circuit editor (T4.5): pq/views/circuit/probes.h; the overlay painter (probe_paint.h).

#include "pq/views/circuit/probes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "pq/graph/inspect.h"
#include "pq/ui/color.h"
#include "pq/ui/lines.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "probe_paint.h"

namespace pq::views::circuit {

bool attributeCost(const PatchEditor::PlayedProgram& played, const PatchPlayer::CostReading& reading,
                   std::span<const graph::NodeId> path, CostMap& out) {
  const graph::Schedule& s = played.schedule;
  const size_t nr = s.regions.size();
  if (reading.generation != played.generation || reading.regionNsPerFrame.size() != nr || reading.units.size() != nr)
    return false;
  if (played.instanceParent.empty()) return false;
  // The shown instance, by call NodeIds from the root.
  uint32_t shown = 0;
  for (const graph::NodeId id : path) {
    uint32_t next = graph::kNone;
    for (uint32_t j = 0; j < played.instanceParent.size() && next == graph::kNone; ++j)
      if (played.instanceParent[j] == shown && played.instanceCall[j] == id) next = j;
    if (next == graph::kNone) return false;
    shown = next;
  }
  out = CostMap{};
  out.totalNs = reading.totalNsPerFrame;
  // Each timing unit's weight: the sum of its ops' estimates.
  std::vector<double> unitWeight(nr, 0.0);
  for (uint32_t r = 0; r < nr; ++r) {
    const uint32_t u = reading.units[r] < nr ? reading.units[r] : r;
    for (uint32_t i = s.regions[r].firstOp; i < s.regions[r].firstOp + s.regions[r].numOps; ++i)
      unitWeight[u] += graph::opCostWeight(s, i);
  }
  std::unordered_map<uint64_t, double> nodes, sums, copies;
  for (uint32_t r = 0; r < nr; ++r) {
    const uint32_t u = reading.units[r] < nr ? reading.units[r] : r;
    const double ns = reading.regionNsPerFrame[u];
    if (!(ns > 0.0) || !(unitWeight[u] > 0.0)) continue;
    for (uint32_t i = s.regions[r].firstOp; i < s.regions[r].firstOp + s.regions[r].numOps; ++i) {
      const graph::Op& op = s.ops[i];
      const double share = ns * graph::opCostWeight(s, i) / unitWeight[u];
      if (op.node >= played.nodeInstance.size()) continue;
      const uint32_t inst = played.nodeInstance[op.node];
      if (inst == shown) {
        const graph::NodeId src = played.nodeSource[op.node];
        if (!src.valid()) continue;
        if (played.nodeWhat[op.node] == graph::NodeOrigin::What::Reducer) sums[src.value] += share;
        else if (op.kind == graph::OpKind::Broadcast) copies[src.value] += share;
        else nodes[src.value] += share;
        continue;
      }
      // Below the shown instance: charged to the call node of the shown patch it is inside.
      uint32_t c = inst;
      while (c < played.instanceParent.size() && played.instanceParent[c] != shown) c = played.instanceParent[c];
      if (c < played.instanceParent.size() && played.instanceCall[c].valid()) nodes[played.instanceCall[c].value] += share;
    }
  }
  auto list = [&](const std::unordered_map<uint64_t, double>& m, std::vector<CostMap::Entry>& v) {
    for (const auto& [id, ns] : m) {
      CostMap::Entry e;
      e.node.value = static_cast<uint32_t>(id);
      e.ns = ns;
      v.push_back(e);
      out.shownNs += ns;
    }
    std::sort(v.begin(), v.end(), [](const CostMap::Entry& a, const CostMap::Entry& b) { return a.node.value < b.node.value; });
  };
  list(nodes, out.nodes);
  list(sums, out.sumsInto);
  list(copies, out.copiesFrom);
  return true;
}

std::string formatProbeValue(float v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", static_cast<double>(v));
  return buf;
}

std::string formatCostNs(double ns) {
  char buf[32];
  if (ns < 10.0) std::snprintf(buf, sizeof buf, "%.2f ns", ns);
  else if (ns < 1000.0) std::snprintf(buf, sizeof buf, "%.1f ns", ns);
  else std::snprintf(buf, sizeof buf, "%.2f \xC2\xB5s", ns / 1000.0);
  return buf;
}

// ---- painting ---------------------------------------------------------------------------------------------------------

void paintProbeReadout(ui::PaintContext& ctx, const ProbeOverlay& o, ui::Vec2 anchor, const ui::IRect& clip) {
  const ui::Theme* th = ctx.res.theme;
  if (!th || !o.readout) return;
  const float w = std::round(ctx.scale.exact(168.0f)), h = std::round(ctx.scale.exact(64.0f));
  const float pad = std::round(ctx.scale.exact(6.0f)), off = std::round(ctx.scale.exact(14.0f));
  // Below right of the pointer, flipped to stay inside the canvas.
  float x = anchor.x + off, y = anchor.y + off;
  if (x + w > static_cast<float>(clip.x + clip.w)) x = anchor.x - off - w;
  if (y + h > static_cast<float>(clip.y + clip.h)) y = anchor.y - off - h;
  x = std::round(std::max(x, static_cast<float>(clip.x)));
  y = std::round(std::max(y, static_cast<float>(clip.y)));
  const ui::Rect box{x, y, x + w, y + h};
  ctx.dl.addRoundedRectFilled(box, ctx.scale.exact(3.0f), th->circuit.probeBg);
  ctx.dl.addRoundedRectStroke(box, ctx.scale.exact(3.0f), th->circuit.probeGrid, 1.0f);
  // Value line.
  float textH = 0.0f;
  if (ctx.res.text && ctx.res.uiFonts) {
    const ui::FontId font = ctx.res.uiFonts->small;
    const std::string label = o.hasData ? o.value : std::string("\xE2\x80\xA6");  // … while the probe compiles
    textH = std::round(ctx.scale.exact(14.0f));
    const float base = y + pad + textH - std::round(ctx.scale.exact(3.0f));
    // The source's name on the left (dimmed), the value right-aligned.
    const float vw = static_cast<float>(ctx.res.text->measure(font, label));
    if (!o.source.empty())
      (void)ctx.res.text->drawTruncated(ctx.dl, font, {x + pad, base}, o.source,
                                        static_cast<int32_t>(std::max(0.0f, w - 3.0f * pad - vw)),
                                        ui::mulAlpha(th->circuit.probeText, 0.6f));
    (void)ctx.res.text->draw(ctx.dl, font, {x + w - pad - vw, base}, label,
                             o.isBool && o.hasData && o.value == "HIGH" ? th->circuit.wireBoolLit : th->circuit.probeText);
  }
  // Mini-scope.
  const ui::Rect sc{x + pad, y + pad + textH + std::round(ctx.scale.exact(2.0f)), x + w - pad, y + h - pad};
  const float sw = sc.x1 - sc.x0, sh = sc.y1 - sc.y0;
  if (sw < 4.0f || sh < 4.0f || o.samples.empty() || !o.hasData) return;
  const uint32_t cols = static_cast<uint32_t>(sw);
  const size_t n = o.samples.size();
  if (o.isBool) {  // a logic trace: high / low per column (high if any sample in it is)
    const float yHi = sc.y0 + 1.0f, yLo = sc.y1 - 1.0f;
    float prevY = -1.0f;
    for (uint32_t c = 0; c < cols; ++c) {
      const size_t a = n * c / cols, b = std::max(a + 1, n * (c + 1) / cols);
      bool high = false;
      for (size_t k = a; k < b && k < n; ++k) high = high || std::signbit(o.samples[k]);
      const float yy = high ? yHi : yLo;
      if (prevY >= 0.0f && prevY != yy) ctx.dl.addVLine(sc.x0 + static_cast<float>(c), std::min(prevY, yy), std::max(prevY, yy) + 1.0f, th->circuit.wireBoolLit);
      ctx.dl.addHLine(sc.x0 + static_cast<float>(c), sc.x0 + static_cast<float>(c) + 1.0f, yy, high ? th->circuit.wireBoolLit : th->circuit.wireBool);
      prevY = yy;
    }
    return;
  }
  float lo = 0.0f, hi = 0.0f;
  bool first = true;
  for (const float v : o.samples)
    if (std::isfinite(v)) {
      lo = first ? v : std::min(lo, v);
      hi = first ? v : std::max(hi, v);
      first = false;
    }
  if (first) return;
  if (lo < 0.0f && hi > 0.0f) {  // around 0: symmetric, with the zero line
    hi = std::max(hi, -lo);
    lo = -hi;
  }
  if (hi - lo < 1e-9f) {  // flat: centred
    hi += 0.5f;
    lo -= 0.5f;
  }
  auto yOf = [&](float v) { return sc.y1 - (v - lo) / (hi - lo) * sh; };
  if (lo < 0.0f && hi > 0.0f) ctx.dl.addHLine(sc.x0, sc.x1, std::round(yOf(0.0f)), th->circuit.probeGrid);
  if (n <= cols) {  // sparse: a polyline through the samples
    std::vector<ui::Vec2> pts;
    pts.reserve(n);
    for (size_t k = 0; k < n; ++k)
      pts.push_back({sc.x0 + sw * static_cast<float>(k) / static_cast<float>(std::max<size_t>(n - 1, 1)),
                     yOf(std::isfinite(o.samples[k]) ? o.samples[k] : 0.0f)});
    ui::smoothPolyline(ctx.dl, pts.data(), static_cast<uint32_t>(pts.size()), th->circuit.probeTrace, ctx.scale.exact(1.25f));
    return;
  }
  // Dense: a min / max band per column, the band's centre as the line.
  std::vector<ui::Vec2> mid;
  mid.reserve(cols);
  for (uint32_t c = 0; c < cols; ++c) {
    const size_t a = n * c / cols, b = std::max(a + 1, n * (c + 1) / cols);
    float mn = 0.0f, mx = 0.0f;
    bool any = false;
    for (size_t k = a; k < b && k < n; ++k)
      if (std::isfinite(o.samples[k])) {
        mn = any ? std::min(mn, o.samples[k]) : o.samples[k];
        mx = any ? std::max(mx, o.samples[k]) : o.samples[k];
        any = true;
      }
    if (!any) continue;
    const float x0 = sc.x0 + static_cast<float>(c);
    ctx.dl.addVLine(x0 + 0.5f, yOf(mx), yOf(mn) + 1.0f, ui::mulAlpha(th->circuit.probeTrace, 0.45f));
    mid.push_back({x0 + 0.5f, yOf(0.5f * (mn + mx))});
  }
  if (mid.size() >= 2)
    ui::smoothPolyline(ctx.dl, mid.data(), static_cast<uint32_t>(mid.size()), th->circuit.probeTrace, ctx.scale.exact(1.0f));
}

void paintCostLabels(ui::PaintContext& ctx, std::span<const CostLabelAt> labels) {
  const ui::Theme* th = ctx.res.theme;
  if (!th || !ctx.res.text || !ctx.res.uiFonts || labels.empty()) return;
  const ui::FontId font = ctx.res.uiFonts->small;
  const float pad = std::round(ctx.scale.exact(3.0f)), lh = std::round(ctx.scale.exact(14.0f));
  // Costliest first; a label that would overlap one already placed is left out (the status line has the total, and
  // zooming in separates them).
  std::vector<uint32_t> order(labels.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return labels[x].heat > labels[y].heat; });
  std::vector<ui::Rect> placed;
  placed.reserve(labels.size());
  for (const uint32_t i : order) {
    const CostLabelAt& l = labels[i];
    const float tw = static_cast<float>(ctx.res.text->measure(font, *l.text));
    const float x = std::round(l.at.x - tw * 0.5f - pad), y = std::round(l.above ? l.at.y - lh - pad : l.at.y + pad);
    const ui::Rect box{x, y, x + tw + 2.0f * pad, y + lh};
    bool free = true;
    for (const ui::Rect& r : placed) free = free && (box.x1 + 1.0f <= r.x0 || r.x1 + 1.0f <= box.x0 || box.y1 + 1.0f <= r.y0 || r.y1 + 1.0f <= box.y0);
    if (!free) continue;
    placed.push_back(box);
    ctx.dl.addRoundedRectFilled(box, ctx.scale.exact(2.0f), th->circuit.costBg);
    // Hot parts stand out: the text goes from the readout colour to the cost colour with the heat.
    const ui::Rgba c = ui::lerp(th->circuit.probeText, th->circuit.cost, std::clamp(0.25f + l.heat * 0.75f, 0.0f, 1.0f));
    (void)ctx.res.text->draw(ctx.dl, font, {x + pad, y + lh - std::round(ctx.scale.exact(3.0f))}, *l.text, c);
  }
}

}  // namespace pq::views::circuit
