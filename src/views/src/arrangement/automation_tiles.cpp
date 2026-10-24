// Automation envelope tiles (T3.x-fix-paint, M3 review H8): an automated band's line is rasterised once into the
// renderer's TileAtlas (pq/ui/tile_atlas.h, T3.8-fix1) in chunks of kEnvTileWidth timeline pixels and drawn as one
// textured quad per chunk, instead of re-emitting its ribbon (thousands of vertices per band) on every repaint.
//
// Chunks live in timeline pixels: column g covers ticks [g / k, (g + 1) / k) at k device px per tick. A chunk is drawn
// at screen x = g + round(lanes.x − origin · k), so scrolling moves whole tiles (the scroll's pixel fraction is
// rounded once, as T3.8-fix1 rounds clip edges) and never invalidates them. A tile is keyed by everything its pixels
// depend on: the lane snapshot (held, so its address is never reused while the tile lives), the exact zoom, the
// band's height and value range, the display scale, the line width and the density path. Colour is the vertex
// colour (an overridden envelope's grey: no rebuild).
//
// The coverage matches the direct paths' look: the ribbon's opaque core of lineW plus a kSmoothFeather ramp, sampled
// at pixel centres as the GPU samples the ribbon (distance to the polyline, unioned by max), and the dense path's
// per-column min/max spans. Tile and direct pixels can differ by antialiasing and up to half a pixel of x phase, so a
// band never mixes them within a frame without repainting: a chunk drawn directly (its tile not built yet: the zoom
// just settled, a new snapshot) is repainted whole once its tile exists (the build damages it).
//
// Tiles are rasterised only in LaneCanvas::onFrame (buildEnvelopeTiles, within a texel budget per frame), never in
// paint, and only while the zoom is settled.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "internal.h"
#include "pq/ui/lines.h"
#include "pq/ui/tile_atlas.h"
#include "pq/views/arrangement/automation.h"

namespace pq::views {

using namespace pq::ui;
namespace au = automation_metrics;

namespace {

constexpr int32_t kEnvTileWidth = 512;           // timeline px per chunk (device px)
constexpr size_t kEnvTileTexelsPerFrame = 1u << 19;
constexpr size_t kEnvTileMaxEntries = 4096;      // cache entries before old ones are dropped
constexpr uint64_t kEnvTileMaxAge = 600;         // frames an unused entry survives a trim

inline int64_t floorDiv(int64_t a, int64_t b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint64_t mix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}
template <class T>
uint64_t bitsOf(T v) noexcept {
  uint64_t b = 0;
  std::memcpy(&b, &v, sizeof(T) < sizeof(b) ? sizeof(T) : sizeof(b));
  return b;
}

// Point-to-segment distance.
inline float segDistance(float px, float py, float ax, float ay, float bx, float by) noexcept {
  const float dx = bx - ax, dy = by - ay;
  const float len2 = dx * dx + dy * dy;
  float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  const float qx = ax + t * dx - px, qy = ay + t * dy - py;
  return std::sqrt(qx * qx + qy * qy);
}

}  // namespace

bool ArrangementAutomation::EnvTileKey::operator==(const EnvTileKey& o) const noexcept {
  return lane == o.lane && chunk == o.chunk && k == o.k && lineW == o.lineW && h == o.h && top == o.top && bottom == o.bottom &&
         disp.scale == o.disp.scale && disp.lo == o.disp.lo && disp.hi == o.disp.hi && dense == o.dense;
}

uint64_t ArrangementAutomation::EnvTileKey::hash() const noexcept {
  uint64_t acc = bitsOf(reinterpret_cast<uintptr_t>(lane));
  acc = mix(acc, bitsOf(chunk));
  acc = mix(acc, bitsOf(k));
  acc = mix(acc, bitsOf(lineW));
  acc = mix(acc, bitsOf(h));
  acc = mix(acc, bitsOf(top));
  acc = mix(acc, bitsOf(bottom));
  acc = mix(acc, static_cast<uint64_t>(disp.scale));
  acc = mix(acc, bitsOf(disp.lo));
  acc = mix(acc, bitsOf(disp.hi));
  acc = mix(acc, dense ? 1u : 0u);
  return acc;
}

// Whether a band's line can come from tiles now, and the key fields common to its chunks.
bool ArrangementAutomation::envTileEligible(const AutomationBand& b, std::span<const doc::Breakpoint> pts, const TileAtlas* tiles) const noexcept {
  return tiles && !view_.zooming() && b.lane && !b.pinned && !pts.empty() && pts.data() == b.lane->points.data() && b.rect.h >= 3 &&
         static_cast<uint32_t>(b.rect.h) <= tiles->maxTileSize();
}

ArrangementAutomation::EnvTileKey ArrangementAutomation::envTileKey(const AutomationBand& b, int64_t chunk, bool dense, float lineW) const {
  EnvTileKey key;
  key.lane = b.lane;
  key.chunk = chunk;
  key.k = view_.devicePxPerTick();
  key.lineW = lineW;
  key.h = b.rect.h;
  key.top = b.top - static_cast<float>(b.rect.y);
  key.bottom = b.bottom - static_cast<float>(b.rect.y);
  key.disp = paramDisplay(*view_.shown(), b.param);
  key.dense = dense;
  return key;
}

double ArrangementAutomation::envTileOffset() const noexcept {
  // Screen x of timeline px 0, rounded once (whole-pixel tiles).
  const double k = view_.devicePxPerTick();
  const double lanesX = static_cast<double>(view_.xOfTick(view_.originTick()));
  return std::floor(lanesX - view_.originTick() * k + 0.5);
}

bool ArrangementAutomation::bandDense(const AutomationBand& b, std::span<const doc::Breakpoint> pts) const noexcept {
  const auto bandFirst = std::lower_bound(pts.begin(), pts.end(), static_cast<Ticks>(std::floor(view_.tickAtX(static_cast<float>(b.rect.x)))),
                                          [](const doc::Breakpoint& bp, Ticks t) { return bp.tick < t; });
  const auto bandLast = std::upper_bound(bandFirst, pts.end(), static_cast<Ticks>(std::ceil(view_.tickAtX(static_cast<float>(b.rect.right())))),
                                         [](Ticks t, const doc::Breakpoint& bp) { return t < bp.tick; });
  return static_cast<float>(bandLast - bandFirst) * 2.0f > static_cast<float>(b.rect.w);
}

bool ArrangementAutomation::paintEnvelopeTiles(PaintContext& ctx, const AutomationBand& b, std::span<const doc::Breakpoint> pts,
                                               const IRect& vis, bool dense, Rgba lineCol, float lineW, double& offset) {
  TileAtlas* tiles = ctx.res.tiles;
  if (!envTileEligible(b, pts, tiles)) return false;
  offset = envTileOffset();
  const auto off = static_cast<int64_t>(offset);
  const int64_t c0 = floorDiv(static_cast<int64_t>(vis.x) - off, kEnvTileWidth);
  const int64_t c1 = floorDiv(static_cast<int64_t>(vis.right()) - 1 - off, kEnvTileWidth);
  const float inv = tiles->uvScale();
  const double fallback = currentParamValue(*view_.shown(), b.param);
  for (int64_t chunk = c0; chunk <= c1; ++chunk) {
    const EnvTileKey key = envTileKey(b, chunk, dense, lineW);
    const auto x0 = static_cast<int32_t>(chunk * kEnvTileWidth + off);
    const auto it = envTiles_.find(key.hash());
    if (it != envTiles_.end() && it->second.key == key) {
      EnvTile& e = it->second;
      e.lastUsed = envTileClock_;
      if (e.state == EnvTile::State::Ready && tiles->valid(e.tile)) {
        tiles->touch(e.tile.page);
        const Rect r{static_cast<float>(x0), static_cast<float>(b.rect.y), static_cast<float>(x0 + kEnvTileWidth),
                     static_cast<float>(b.rect.y + b.rect.h)};
        const Vec2 uv0{static_cast<float>(e.tile.x) * inv, static_cast<float>(e.tile.y) * inv};
        const Vec2 uv1{static_cast<float>(e.tile.x + e.tile.w) * inv, static_cast<float>(e.tile.y + e.tile.h) * inv};
        ctx.dl.addImage(TileAtlas::texture(e.tile.page), r, uv0, uv1, lineCol);
        ++stats_.envTilesDrawn;
        continue;
      }
      if (e.state == EnvTile::State::Failed) {  // no room in the atlas for this key: direct, without asking again
        paintEnvelopeDirect(ctx, b, pts, fallback, intersect(vis, {x0, b.rect.y, kEnvTileWidth, b.rect.h}), dense, lineCol, lineW);
        continue;
      }
    }
    // Not built yet: this chunk directly; onFrame builds the tile and repaints the chunk from it.
    ++stats_.envTilesMissing;
    envTilesWanted_ = true;
    paintEnvelopeDirect(ctx, b, pts, fallback, intersect(vis, {x0, b.rect.y, kEnvTileWidth, b.rect.h}), dense, lineCol, lineW);
  }
  return true;
}

bool ArrangementAutomation::rasteriseEnvTile(TileAtlas& tiles, const AutomationBand& b, std::span<const doc::Breakpoint> pts,
                                             const EnvTileKey& key, EnvTile& e) {
  e.key = key;
  e.state = EnvTile::State::Failed;
  if (!tiles.allocate(static_cast<uint32_t>(kEnvTileWidth), static_cast<uint32_t>(key.h), e.tile)) {
    ++stats_.envTileFailures;
    return false;
  }
  uint8_t* dst = tiles.write(e.tile);
  const uint32_t pitch = TileAtlas::rowPitch(e.tile);
  uint8_t* base = dst + pitch + 1;
  const int32_t W = kEnvTileWidth, H = key.h;
  const double k = key.k;
  const auto G0 = static_cast<double>(key.chunk * kEnvTileWidth);
  const double fallback = currentParamValue(*view_.shown(), b.param);
  const auto yOf = [&](double v) {
    return static_cast<float>(static_cast<double>(key.bottom) - displayNorm(key.disp, v) * static_cast<double>(key.bottom - key.top));
  };
  if (key.dense) {
    // Per column the span from the lowest to the highest value (as the direct dense path), rows by pixel centre.
    EnvelopeCursor cur(pts, fallback);
    double tl = G0 / k;
    auto it = std::lower_bound(pts.begin(), pts.end(), static_cast<Ticks>(std::floor(tl)),
                               [](const doc::Breakpoint& bp, Ticks t) { return bp.tick < t; });
    double vl = cur.at(tl);
    for (int32_t col = 0; col < W; ++col) {
      const double tr = (G0 + static_cast<double>(col + 1)) / k;
      double vmin = vl, vmax = vl;
      for (; it != pts.end() && static_cast<double>(it->tick) < tr; ++it) {
        if (static_cast<double>(it->tick) < tl) continue;
        vmin = std::min(vmin, static_cast<double>(it->value));
        vmax = std::max(vmax, static_cast<double>(it->value));
      }
      const double vr = cur.at(tr);
      vmin = std::min(vmin, vr);
      vmax = std::max(vmax, vr);
      vl = vr;
      tl = tr;
      const float ya = yOf(vmin), yb = vmax == vmin ? ya : yOf(vmax);
      const float y0 = std::min(ya, yb) - key.lineW * 0.5f, y1 = std::max(ya, yb) + key.lineW * 0.5f;
      const int32_t r0 = std::max(0, static_cast<int32_t>(std::ceil(y0 - 0.5f))), r1 = std::min(H, static_cast<int32_t>(std::ceil(y1 - 0.5f)));
      for (int32_t y = r0; y < r1; ++y) base[static_cast<size_t>(y) * pitch + static_cast<size_t>(col)] = 255;
    }
  } else {
    // The polyline (a vertex per breakpoint, curved segments in ≤ kMaxChordPx chords) and, at every pixel centre
    // near it, the ribbon's coverage: 1 inside the core (lineW / 2), falling linearly to 0 over the feather.
    const float hw = key.lineW * 0.5f, f = kSmoothFeather, reach = hw + f;
    const double margin = static_cast<double>(reach) + 2.0;
    // The polyline runs from the last breakpoint before the chunk to the first one after it, so every straight
    // segment is the line between its breakpoints exactly as the direct path draws it (an end vertex evaluated at the
    // chunk edge would bend a segment whose display scale is not linear, e.g. dB on the fader scale).
    double t0 = (G0 - margin) / k, t1 = (G0 + static_cast<double>(W) + margin) / k;
    {
      auto it = std::upper_bound(pts.begin(), pts.end(), t0, [](double t, const doc::Breakpoint& bp) { return t < static_cast<double>(bp.tick); });
      if (it != pts.begin()) t0 = std::min(t0, static_cast<double>(std::prev(it)->tick));
      auto jt = std::lower_bound(pts.begin(), pts.end(), t1, [](const doc::Breakpoint& bp, double t) { return static_cast<double>(bp.tick) < t; });
      if (jt != pts.end()) t1 = std::max(t1, static_cast<double>(jt->tick));
    }
    envelopeVertices(pts, fallback, t0, t1, static_cast<double>(au::kMaxChordPx) / k, verts_);
    poly_.clear();
    for (const EnvelopeVertex& v : verts_) poly_.push_back({static_cast<float>(v.tick * k - G0), yOf(v.value)});
    for (size_t i = 0; i + 1 < poly_.size(); ++i) {
      const Vec2 a = poly_[i], z = poly_[i + 1];
      const float xmin = std::min(a.x, z.x), xmax = std::max(a.x, z.x);
      const int32_t cx0 = std::max(0, static_cast<int32_t>(std::floor(xmin - reach)));
      const int32_t cx1 = std::min(W - 1, static_cast<int32_t>(std::ceil(xmax + reach)));
      const float dx = z.x - a.x;
      const auto yAt = [&](float x) { return dx != 0.0f ? a.y + (z.y - a.y) * (x - a.x) / dx : a.y; };
      for (int32_t cx = cx0; cx <= cx1; ++cx) {
        const float xc = static_cast<float>(cx) + 0.5f;
        float ylo, yhi;
        if (dx == 0.0f) {
          ylo = std::min(a.y, z.y);
          yhi = std::max(a.y, z.y);
        } else {
          const float ya = yAt(std::clamp(xc - reach, xmin, xmax)), yb = yAt(std::clamp(xc + reach, xmin, xmax));
          ylo = std::min(ya, yb);
          yhi = std::max(ya, yb);
        }
        const int32_t r0 = std::max(0, static_cast<int32_t>(std::floor(ylo - reach)));
        const int32_t r1 = std::min(H - 1, static_cast<int32_t>(std::ceil(yhi + reach)));
        for (int32_t ry = r0; ry <= r1; ++ry) {
          const float d = segDistance(xc, static_cast<float>(ry) + 0.5f, a.x, a.y, z.x, z.y);
          const float cov = std::clamp((reach - d) / f, 0.0f, 1.0f);
          if (cov <= 0.0f) continue;
          uint8_t& px = base[static_cast<size_t>(ry) * pitch + static_cast<size_t>(cx)];
          px = std::max(px, static_cast<uint8_t>(cov * 255.0f + 0.5f));
        }
      }
    }
  }
  e.state = EnvTile::State::Ready;
  ++stats_.envTilesBuilt;
  return true;
}

bool ArrangementAutomation::buildEnvelopeTiles(TileAtlas* tiles, const IRect& canvas, const IRect& viewport) {
  envTilesWanted_ = false;
  if (!tiles || view_.zooming()) return true;
  ++envTileClock_;
  UiTree* tree = view_.tree();
  const IRect c = intersect(canvas, viewport);
  if (c.empty() || !tree) return true;
  const UiScale s = tree->uiScale();
  const float lineW = std::max(1.0f, au::kLineWidth * s.factor);
  const double offset = envTileOffset();
  const auto off = static_cast<int64_t>(offset);
  const int32_t top0 = canvas.y;
  const int32_t localTop = c.y - top0, localBottom = c.bottom() - top0;
  // The visible bands of the lanes canvas (automation-mode rows, then lanes), as paintLanes walks them.
  std::vector<AutomationBand>& bands = buildBands_;
  bands.clear();
  if (mode_) {
    const auto rows = view_.rows();
    auto it = std::upper_bound(rows.begin(), rows.end(), localTop, [](int32_t y, const ArrangementRow& r) { return y < r.y + r.h; });
    for (; it != rows.end() && it->y < localBottom; ++it)
      if (const auto b = rowBand(static_cast<int32_t>(it - rows.begin()), false)) bands.push_back(*b);
  }
  const auto lanes = view_.laneRows();
  auto lit = std::upper_bound(lanes.begin(), lanes.end(), localTop, [](int32_t y, const AutomationLaneRow& r) { return y < r.y + r.h; });
  for (; lit != lanes.end() && lit->y < localBottom; ++lit) bands.push_back(laneBand(static_cast<size_t>(lit - lanes.begin())));

  // Pass 1: the tiles in use stay (allocations below never evict them); pass 2 builds the missing ones.
  bool done = true;
  size_t texels = 0;
  bool builtAny = false;
  for (int pass = 0; pass < 2; ++pass)
    for (const AutomationBand& b : bands) {
      const std::span<const doc::Breakpoint> pts = pointsOf(b);
      if (!envTileEligible(b, pts, tiles)) continue;
      const IRect vis = intersect(b.rect, c);
      if (vis.empty()) continue;
      const bool dense = bandDense(b, pts);
      const int64_t c0 = floorDiv(static_cast<int64_t>(vis.x) - off, kEnvTileWidth);
      const int64_t c1 = floorDiv(static_cast<int64_t>(vis.right()) - 1 - off, kEnvTileWidth);
      for (int64_t chunk = c0; chunk <= c1; ++chunk) {
        const EnvTileKey key = envTileKey(b, chunk, dense, lineW);
        const uint64_t h = key.hash();
        auto it = envTiles_.find(h);
        const bool have = it != envTiles_.end() && it->second.key == key &&
                          (it->second.state == EnvTile::State::Failed || tiles->valid(it->second.tile));
        if (pass == 0) {
          if (have && it->second.state == EnvTile::State::Ready) tiles->touch(it->second.tile.page);
          continue;
        }
        if (have) {
          it->second.lastUsed = envTileClock_;
          continue;
        }
        if (builtAny && texels >= kEnvTileTexelsPerFrame) {
          done = false;
          continue;
        }
        EnvTile& e = envTiles_[h];
        e = EnvTile{};
        e.lastUsed = envTileClock_;
        // Hold the lane snapshot the key points at (its address stays unique while the tile lives).
        if (const doc::Track* owner = doc::findTrackPtr(*view_.shown(), b.owner))
          for (const doc::LanePtr& lp : owner->lanes)
            if (lp.get() == b.lane) e.hold = lp;
        (void)rasteriseEnvTile(*tiles, b, pts, key, e);
        texels += static_cast<size_t>(kEnvTileWidth) * static_cast<size_t>(key.h);
        builtAny = true;
        // The chunk was drawn directly (or not at all): repaint it from the tile.
        const IRect r = intersect(intersect(IRect{static_cast<int32_t>(chunk * kEnvTileWidth + off), b.rect.y, kEnvTileWidth, b.rect.h}, b.rect), c);
        if (!r.empty()) tree->damage().add(r.toRect());
      }
    }
  // Bound the cache: entries unused for kEnvTileMaxAge builds go once it is large.
  if (envTiles_.size() > kEnvTileMaxEntries) {
    const uint64_t keepFrom = envTileClock_ > kEnvTileMaxAge ? envTileClock_ - kEnvTileMaxAge : 0;
    std::erase_if(envTiles_, [&](const auto& kv) { return kv.second.lastUsed < keepFrom; });
  }
  return done;
}

}  // namespace pq::views
