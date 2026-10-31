// Audio clips in the arrangement (T5.6): the waveform (from the sample's overview, pq/views/waveforms.h, drawn by
// ui::drawWaveform: scaled by the clip gain, mirrored when reversed), fade curves, the crossfade X, badges (transpose,
// detune, warp, reverse, gain), handles (fades, gain) and the red "missing" overlay.
//
// The waveform of a clip is cached like a note preview (T3.8-fix1): rasterised once per (clip snapshot, device px per
// tick, body height, overview progress) into the renderer's TileAtlas and drawn as one quad afterwards; tiles are built
// in LaneCanvas::onFrame within the texel budget once the zoom has settled. Without its tile (no GPU, a drag preview,
// a clip wider than a tile, deep zoom where the samples themselves are drawn) the clip is drawn directly, only over
// its visible columns. Overlays always go to Model::overlay and are emitted after the row's tile quads.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../common/waveform_data.h"
#include "lane_paint.h"
#include "pq/core/automation_curve.h"
#include "pq/core/clip_time.h"
#include "pq/document/audio_clip.h"
#include "pq/ui/lines.h"
#include "pq/ui/waveform.h"
#include "pq/views/waveforms.h"

namespace pq::views::arrangement {

using namespace pq::ui;
namespace am = arrangement_metrics;

namespace {

// Source frames per device px at the clip start (the level of detail; exact for constant tempo and RePitch).
double framesPerPx(const AudioLayout& L, const doc::AudioClip& c, const TempoMap& tm, double k) noexcept {
  if (c.warp == doc::AudioWarp::RePitch) return L.fpt / k;
  const double secPerTick = tm.ticksToSeconds(c.start + kPpq) - tm.ticksToSeconds(c.start);
  return L.rate * L.ratio * secPerTick / static_cast<double>(kPpq) / k;
}

bool wantsAudioTile(const TileAtlas& tiles, int32_t width, int32_t bodyH) noexcept {
  return bodyH >= 3 && width >= 1 && static_cast<uint32_t>(width) <= tiles.maxTileSize() &&
         static_cast<uint32_t>(bodyH) <= tiles.maxTileSize();
}

int32_t localWidth(Ticks length, double k) noexcept {
  return std::max<int32_t>(1, static_cast<int32_t>(std::clamp(std::floor(static_cast<double>(length) * k + 0.5), 1.0, 1073741824.0)));
}

// The draw of one linear piece (clip-local px [xa, xb) showing source frames [fa, fb), mirrored when reversed) with
// the clip's left edge at x `x0`. Columns are the whole px whose centre lies in the piece. False: no column.
bool pieceDraw(const AudioPiece& p, float x0, float by0, float by1, float gain, Rgba col, float lo, float hi, WaveformDraw& d) {
  const double ia = std::ceil(p.xa - 0.5), ib = std::ceil(p.xb - 0.5);
  if (ib <= ia || !(p.fb > p.fa)) return false;
  const double s = (p.fb - p.fa) / (p.xb - p.xa);
  if (!p.reverse) {
    d.frame0 = p.fa + (ia - p.xa) * s;
    d.frame1 = p.fa + (ib - p.xa) * s;
  } else {
    d.frame1 = p.fb - (ia - p.xa) * s;
    d.frame0 = p.fb - (ib - p.xa) * s;
  }
  d.rect = {x0 + static_cast<float>(ia), by0, x0 + static_cast<float>(ib), by1};
  d.gain = gain;
  d.reverse = p.reverse;
  d.colour = col;
  d.clipX0 = lo;
  d.clipX1 = hi;
  return true;
}

WaveformStats drawPiece(DrawList& dl, const WaveformData& data, const AudioPiece& p, float x0, float by0, float by1, float gain,
                        Rgba col, float lo, float hi) {
  WaveformDraw d;
  if (!pieceDraw(p, x0, by0, by1, gain, col, lo, hi, d)) return {};
  return drawWaveform(dl, data, d);
}

void queueAudioTile(LanePaint& lp, const TileAtlas& tiles, const AudioTile& t, float cx0, float vx0, float vx1, float by0,
                    int32_t bodyH, Rgba col) {
  const double ratio = t.k / lp.k;  // tile texels per device px
  const float x1 = std::min(vx1, cx0 + static_cast<float>(static_cast<double>(t.width) / ratio));
  if (x1 <= vx0) return;
  const float inv = tiles.uvScale();
  const auto u = [&](float x) {
    return (static_cast<float>(t.tile.x) + static_cast<float>(static_cast<double>(x - cx0) * ratio)) * inv;
  };
  lp.m.tileQuads.push_back({t.tile.page, vx0, by0, x1, by0 + static_cast<float>(bodyH), u(vx0), static_cast<float>(t.tile.y) * inv, u(x1),
                            static_cast<float>(t.tile.y + static_cast<uint32_t>(t.bodyH)) * inv, col});
}

// Rasterises the clip's waveform at (k, bodyH) into a new tile. False when the atlas has no room this frame.
bool buildAudioTile(TileAtlas& tiles, AudioLayout& L, const media::WaveformOverview& ov, double k, int32_t bodyH, Model& m) {
  const doc::AudioClip& c = *L.clip;
  AudioTile& t = L.tile;
  const int32_t width = localWidth(c.length, k);
  t = AudioTile{};
  t.k = k;
  t.width = width;
  t.bodyH = bodyH;
  t.built = ov.builtFrames();
  TileAtlas::Tile tile;
  if (!tiles.allocate(static_cast<uint32_t>(width), static_cast<uint32_t>(bodyH), tile)) {
    t.state = PreviewTile::State::Failed;
    ++m.stats.tileFailures;
    return false;
  }
  WaveformLevel lv[kMaxOverviewLevels];
  const WaveformData data = waveformData(ov, nullptr, lv);
  uint8_t* dst = tiles.write(tile);
  const uint32_t pitch = TileAtlas::rowPitch(tile);
  // The strips' coverage written directly (ui::rasterWaveform: the pixels drawWaveform covers, without triangles).
  forEachAudioPiece(c, L, *L.tempo, k, 0.0, static_cast<double>(width), [&](const AudioPiece& p) {
    WaveformDraw d;
    if (pieceDraw(p, 0.0f, 0.0f, static_cast<float>(bodyH), clipGainLinear(c.gainDb), rgba(255, 255, 255), -3e38f, 3e38f, d))
      (void)rasterWaveform(data, d, dst + pitch + 1, static_cast<uint32_t>(width), static_cast<uint32_t>(bodyH), pitch);
  });
  t.tile = tile;
  t.state = PreviewTile::State::Ready;
  ++m.stats.tilesBuilt;
  return true;
}

// The badge text of a clip: transpose / detune (warp Off), warp, reverse, gain.
std::string badgeText(const doc::AudioClip& c) {
  std::string s;
  char buf[48];
  auto add = [&](const char* t) {
    if (!s.empty()) s += "  ";
    s += t;
  };
  if (c.warp == doc::AudioWarp::Off) {
    switch (c.transpose.unit) {
      case doc::TransposeUnit::Cents:
        if (c.transpose.cents != 0.0) {
          const double st = c.transpose.cents / 100.0;
          if (std::abs(st - std::round(st)) < 1e-9) std::snprintf(buf, sizeof buf, "%+d st", static_cast<int>(std::lround(st)));
          else std::snprintf(buf, sizeof buf, "%+.1f c", c.transpose.cents);
          add(buf);
        }
        break;
      case doc::TransposeUnit::Steps:
        if (c.transpose.steps != 0) {
          std::snprintf(buf, sizeof buf, "%+d stp", c.transpose.steps);
          add(buf);
        }
        break;
      case doc::TransposeUnit::Ratio:
        if (!(c.transpose.ratio == tuning::Rational{1})) {
          std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(c.transpose.ratio.num()),
                        static_cast<long long>(c.transpose.ratio.den()));
          add(buf);
        }
        break;
    }
    if (c.detuneCents != 0.0f) {
      std::snprintf(buf, sizeof buf, "%+.0f c", static_cast<double>(c.detuneCents));
      add(buf);
    }
  } else {
    std::snprintf(buf, sizeof buf, "RP %.0f", c.sourceBpm);
    add(buf);
  }
  if (c.reverse) add("Rev");
  if (c.gainDb != 0.0f) {
    if (c.gainDb <= doc::kMinClipGainDb) std::snprintf(buf, sizeof buf, "-inf dB");
    else std::snprintf(buf, sizeof buf, "%+.1f dB", static_cast<double>(c.gainDb));
    add(buf);
  }
  return s;
}

// Fade shading + curve (fade-in from the left edge, fade-out to the right edge), the crossfade X and the handles, into
// `o` (the overlay list).
void paintOverlays(DrawList& o, LanePaint& lp, const doc::AudioClip& c, const ClipCols& cc, float by0, float by1, bool handles,
                   int hover) {
  const float scale = lp.ctx.scale.factor;
  const Rgba shade = rgba(0, 0, 0, 80);
  const Rgba line = rgba(255, 255, 255, 190);
  const float lw = std::max(1.0f, std::round(scale));
  const AudioFadesPx f = audioFadesPx(c, lp.k);
  Vec2 pts[65];
  auto fade = [&](float x0, float w, float curve, bool in) {
    if (w < 1.0f) return;
    const int n = std::clamp(static_cast<int>(w / 3.0f), 2, 64);
    for (int i = 0; i <= n; ++i) {
      const double x = static_cast<double>(i) / n;
      const double g = in ? automationShape(x, curve) : 1.0 - automationShape(x, curve);
      pts[i] = {x0 + w * static_cast<float>(x), by1 - static_cast<float>(g) * (by1 - by0)};
    }
    // Shade above the curve: one quad per segment, between the top of the body and the curve.
    const DrawList::PrimSpan sp = o.primReserveSpan(6 * static_cast<uint32_t>(n), 4 * static_cast<uint32_t>(n));
    const Vec2 uv = o.whiteUv();
    for (int i = 0; i < n; ++i) {
      Vertex* v = sp.v + 4 * i;
      v[0] = {{pts[i].x, by0}, uv, shade};
      v[1] = {{pts[i + 1].x, by0}, uv, shade};
      v[2] = {pts[i + 1], uv, shade};
      v[3] = {pts[i], uv, shade};
      uint32_t* ix = sp.i + 6 * i;
      const uint32_t b = sp.base + 4 * static_cast<uint32_t>(i);
      ix[0] = b;
      ix[1] = b + 1;
      ix[2] = b + 2;
      ix[3] = b;
      ix[4] = b + 2;
      ix[5] = b + 3;
    }
    smoothPolyline(o, pts, static_cast<uint32_t>(n + 1), line, lw);
  };
  if (c.crossfadeIn == 0) fade(cc.cx0, f.in, c.fadeIn.curve, true);
  fade(cc.cx1 - f.out, f.out, c.fadeOut.curve, false);
  if (c.crossfadeIn > 0) {  // the X centred on the boundary (both clips' parts of the window)
    const float xa = cc.cx0 - static_cast<float>(static_cast<double>(c.crossfadeIn / 2) * lp.k);
    const float xb = cc.cx0 + static_cast<float>(static_cast<double>(c.crossfadeIn - c.crossfadeIn / 2) * lp.k);
    const Vec2 a[2] = {{xa, by0}, {xb, by1}}, b[2] = {{xa, by1}, {xb, by0}};
    smoothPolyline(o, a, 2, line, lw);
    smoothPolyline(o, b, 2, line, lw);
  }
  if (!handles) return;
  const AudioHandleRects h = audioHandles(c, cc.cx0, cc.cx1, by0, by1, lp.k, scale);
  if (!h.show) return;
  const Rgba fill = rgba(235, 235, 235), lit = rgba(255, 210, 90);
  o.addRectFilled(h.fadeIn, hover == kZoneFadeIn ? lit : fill);
  o.addRectFilled(h.fadeOut, hover == kZoneFadeOut ? lit : fill);
  o.addRectFilled(h.gain, hover == kZoneGain ? lit : fill);
}

}  // namespace

void paintAudioClip(LanePaint& lp, const ArrangementRow& row, const doc::AudioClipPtr& clipPtr, const ClipGeom& g, float top,
                    float bottom, bool trackActive, bool selected, int hover) {
  const doc::AudioClip& own = *clipPtr;
  ClipCols cc;
  if (!clipColumns(lp, g.start, g.length, cc)) return;
  ++lp.m.stats.clipsPainted;
  ViewContext& ctx = lp.view.context();
  DrawList& dl = lp.ctx.dl;
  const bool available = ctx.sampleAvailable(own.sample);
  const bool active = own.active && trackActive;
  const Rgba colour = active ? rgbOf(own.colour) : inactiveColour();
  const float titleB = paintClipChrome(lp, cc, top, bottom, colour, own.name);

  // Badges, right-aligned in the title strip when they fit beside the name.
  if (lp.font != kNoFont && lp.ctx.res.text) {
    const std::string b = available ? badgeText(own) : std::string("Missing");
    if (!b.empty()) {
      TextSystem& ts = *lp.ctx.res.text;
      const TextRun run = ts.shape(lp.font, b);
      const float pad = static_cast<float>(lp.ctx.scale.px(4.0f));
      const float x = std::min(cc.cx1, static_cast<float>(lp.r.right())) - pad - static_cast<float>(run.advance);
      if (x >= cc.cx0 + static_cast<float>(lp.ctx.scale.px(40.0f)) && x >= static_cast<float>(lp.r.x) &&
          x + static_cast<float>(run.advance) <= static_cast<float>(lp.c.right()) + pad)
        TextSystem::drawRun(dl, run, {std::floor(x), top + static_cast<float>(lp.baselineOff)},
                            available ? withAlpha(lp.m.clipTextColour(lp.th, colour), 170) : rgba(255, 90, 80));
    }
  }

  const float by0 = titleB, by1 = bottom - 1.0f;
  const int32_t bodyH = static_cast<int32_t>(by1 - by0);
  if (!available) {  // the red "missing" overlay over the body (the clip plays silence)
    if (by1 > by0) dl.addRectFilled({cc.vx0, by0, cc.vx1, by1}, rgba(200, 40, 35, 120));
    paintClipBorder(lp, cc, top, bottom, titleB, selected, hover >= -1 && hover <= 1 ? hover : kNoHover, false);
    return;
  }

  // The clip as drawn: its own geometry, or a drag preview's (moved / trimmed: a temporary copy, drawn directly).
  AudioLayout& base = lp.m.audioLayout(row, clipPtr);
  const bool ownContent = g.start == own.start && g.length == own.length;
  AudioLayout tmp;
  const AudioLayout* L = &base;
  if (!ownContent) {
    const doc::Timeline& tl = *lp.p.timeline;
    doc::AudioClip moved = g.length == own.length ? doc::audioMovedTo(tl, own, g.start)
                                                  : doc::audioWithBounds(tl, own, g.start, g.start + g.length, base.rate, base.ratio);
    tmp = base;
    tmp.tile = {};
    tmp.clip = std::make_shared<const doc::AudioClip>(std::move(moved));
    tmp.startSec = tl.tempoMap.ticksToSeconds(g.start);
    L = &tmp;
  }
  const doc::AudioClip& c = *L->clip;
  const Rgba waveCol = clipNoteColour(colour);
  bool deferred = false;
  const media::WaveformOverview* ov = ctx.waveforms().overview(own.sample);
  const bool detailed = cc.cx1 - cc.cx0 >= static_cast<float>(lp.ctx.scale.px(4.0f)) && bodyH >= 3;
  if (ov && detailed) {
    TileAtlas* tiles = lp.ctx.res.tiles;
    const double fpp = framesPerPx(*L, c, *L->tempo, lp.k);
    WaveformLevel lv[kMaxOverviewLevels];
    WaveformData data = waveformData(*ov, nullptr, lv);
    const bool sampleLevel = waveformLevelFor(data, fpp) < 0;
    const int32_t width = localWidth(c.length, lp.k);
    bool direct = true;
    if (tiles && ownContent && !sampleLevel && wantsAudioTile(*tiles, width, bodyH)) {
      AudioTile& t = base.tile;
      const bool ready = t.state == PreviewTile::State::Ready && tiles->valid(t.tile);
      if (t.matches(lp.k, bodyH, ov->builtFrames()) && ready) {
        queueAudioTile(lp, *tiles, t, cc.cx0, cc.vx0, cc.vx1, by0, bodyH, waveCol);
        tiles->touch(t.tile.page);
        ++lp.m.stats.tilesDrawn;
        deferred = true;
        direct = false;
      } else if (!(t.matches(lp.k, bodyH, ov->builtFrames()) && t.state == PreviewTile::State::Failed)) {
        ++lp.m.stats.tilesMissing;
        lp.m.tilesMissing = true;
        if (lp.view.zooming()) lp.m.tilesAfterZoom = true;
        if (ready && t.bodyH == bodyH) {  // the previous zoom's tile, stretched, until onFrame has built this one
          queueAudioTile(lp, *tiles, t, cc.cx0, cc.vx0, cc.vx1, by0, bodyH, waveCol);
          tiles->touch(t.tile.page);
          ++lp.m.stats.tilesStale;
          lp.m.approxDrawn = true;
          deferred = true;
          direct = false;
        }
      }
    }
    if (direct) {
      // Only the visible columns; at deep zoom the samples themselves (a window read in the background meanwhile).
      const double lo = static_cast<double>(cc.vx0 - cc.cx0), hi = static_cast<double>(cc.vx1 - cc.cx0);
      if (sampleLevel) {
        double f0 = 1e300, f1 = -1e300;
        forEachAudioPiece(c, *L, *L->tempo, lp.k, lo, hi, [&](const AudioPiece& p) {
          f0 = std::min(f0, p.fa);
          f1 = std::max(f1, p.fb);
        });
        if (f1 > f0)
          if (const media::SampleWindow* w = ctx.waveforms().window(own.sample, static_cast<int64_t>(std::floor(f0)) - 2,
                                                                    static_cast<int64_t>(std::ceil(f1)) + 2))
            data = waveformData(*ov, w, lv);
      }
      forEachAudioPiece(c, *L, *L->tempo, lp.k, lo, hi, [&](const AudioPiece& p) {
        const WaveformStats st = drawPiece(dl, data, p, cc.cx0, by0, by1, clipGainLinear(c.gainDb), waveCol, cc.vx0, cc.vx1);
        lp.m.stats.waveColumns += st.columns;
      });
      ++lp.m.stats.wavesDirect;
    }
  }
  paintClipBorder(lp, cc, top, bottom, titleB, selected, hover >= -1 && hover <= 1 ? hover : kNoHover, deferred);
  const bool handles = selected || hover != kNoHover;
  paintOverlays(lp.m.overlay, lp, c, cc, by0, by1, handles, hover);
}

bool buildAudioTiles(LaneCanvas& canvas, ArrangementView& view, TileAtlas& tiles, const IRect& visible, size_t& texels, bool& builtOne) {
  Model& m = view.model();
  const IRect r = canvas.rect();
  const IRect c = intersect(r, visible);
  if (c.empty() || !m.shown) return true;
  const UiScale s = canvas.tree() ? canvas.tree()->uiScale() : UiScale{};
  const double k = view.devicePxPerTick();
  const double origin = view.originTick();
  const auto from = static_cast<Ticks>(std::floor(origin + (static_cast<double>(c.x - r.x) - 1.0) / k));
  const auto to = static_cast<Ticks>(std::ceil(origin + (static_cast<double>(c.right() - r.x) + 1.0) / k));
  const int32_t titleH = s.px(am::kClipTitleHeight);
  const auto rows = view.rows();
  const int32_t localTop = c.y - r.y, localBottom = c.bottom() - r.y;
  const auto first = std::upper_bound(rows.begin(), rows.end(), localTop,
                                      [](int32_t y, const ArrangementRow& row) { return y < row.y + row.h; });
  ViewContext& ctx = view.context();
  for (auto it = first; it != rows.end() && it->y < localBottom; ++it) {
    if (it->group || it->track->kind != doc::TrackKind::Audio || it->track->clips.empty()) continue;
    // The body as paintAudioClip computes it: [top + titleH, bottom − 1) of the clip's [y + 1, y + h − 1).
    const int32_t bodyH = it->h - 3 - titleH;
    if (bodyH < 3) continue;
    const doc::Track& t = *it->track;
    const ClipIndex& ix = m.clips(t);
    const auto [a, b] = ix.visible(from, to);
    for (size_t j = a; j < b; ++j) {
      const doc::AudioClipPtr& clip = t.clips[j].audio;
      if (!clip || !ctx.sampleAvailable(clip->sample)) continue;
      const media::WaveformOverview* ov = ctx.waveforms().overview(clip->sample);
      if (!ov) continue;
      AudioLayout& L = m.audioLayout(*it, clip);
      const int32_t width = localWidth(clip->length, k);
      if (!wantsAudioTile(tiles, width, bodyH)) continue;
      WaveformLevel lv[kMaxOverviewLevels];
      if (waveformLevelFor(waveformData(*ov, nullptr, lv), framesPerPx(L, *clip, *L.tempo, k)) < 0) continue;
      const AudioTile& at = L.tile;
      if (at.matches(k, bodyH, ov->builtFrames()) &&
          (at.state == PreviewTile::State::Failed || (at.state == PreviewTile::State::Ready && tiles.valid(at.tile)))) {
        if (at.state == PreviewTile::State::Ready) tiles.touch(at.tile.page);
        continue;
      }
      const size_t area = static_cast<size_t>(width + 2) * static_cast<size_t>(bodyH + 2);
      if (builtOne && texels + area > am::kTileBuildTexelsPerFrame) return false;
      (void)buildAudioTile(tiles, L, *ov, k, bodyH, m);
      texels += area;
      builtOne = true;
    }
  }
  return true;
}

}  // namespace pq::views::arrangement
