// Audio clips in the arrangement (T5.6): the waveform (from the sample's overview, pq/views/waveforms.h, drawn by
// ui::drawWaveform: SX.3: multiplied by the clip's envelope — gain, fades, crossfades, the clip player's law — and
// mapped through its warp markers; mirrored when reversed), fade curves, the crossfade X, the processing tag (SX.3:
// "WARP · Beats +3 st", "RE-PITCH", ...; a corner mark on a short clip), handles (fades, gain) and the hatched
// "missing" body.
//
// The waveform of a clip is cached like a note preview (T3.8-fix1): rasterised once per (clip snapshot, device px per
// tick, body height, overview progress) into the renderer's TileAtlas and drawn as one quad afterwards; tiles are built
// in LaneCanvas::onFrame within the texel budget once the zoom has settled. Without its tile (no GPU, a drag preview,
// a clip wider than a tile, deep zoom where the samples themselves are drawn) the clip is drawn directly, only over
// its visible columns. Overlays always go to Model::overlay and are emitted after the row's tile quads.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "../common/waveform_data.h"
#include "lane_paint.h"
#include "pq/core/clip_envelope.h"
#include "pq/core/clip_time.h"
#include "pq/document/audio_clip.h"
#include "pq/ui/lines.h"
#include "pq/ui/waveform.h"
#include "pq/views/waveforms.h"

namespace pq::views::arrangement {

using namespace pq::ui;
namespace am = arrangement_metrics;

namespace {

// Source frames per device px at the clip start (the level of detail; exact for constant tempo and RePitch). Warped:
// the warp law's least slope (its finest detail).
double framesPerPx(const AudioLayout& L, const doc::AudioClip& c, const TempoMap& tm, double k, const WaveSource& src = {}) noexcept {
  if (src.rendered) {  // SX.4: the render's frames follow song time
    const double secPerTick = tm.ticksToSeconds(c.start + kPpq) - tm.ticksToSeconds(c.start);
    return src.rendered->rate * secPerTick / static_cast<double>(kPpq) / k;
  }
  if (doc::isWarped(c.warp)) return L.fptMin / k;
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

// SX.3: a clip's envelope over device columns [x0, x0 + n) (g null: flat, only the clip gain applies there).
struct ColumnEnvelope {
  const float* g = nullptr;
  int32_t x0 = 0;
  uint32_t n = 0;
};

// The envelope's shape at the centres of device columns [ca, cb) of a clip whose left edge is at device x `cx0` (a
// whole px), into a scratch array; flat over them: none (the clip gain is WaveformDraw::gain either way). UI thread.
ColumnEnvelope columnEnvelope(const AudioEnvelope& env, double cx0, double k, double ca, double cb) {
  ColumnEnvelope out;
  if (!(cb > ca) || !(k > 0.0)) return out;
  const double t0 = env.bodyStart + (ca - cx0) / k, t1 = env.bodyStart + (cb - cx0) / k;
  if (env.flatOver(t0, t1)) return out;
  static std::vector<float> scratch;  // (the UI thread's; grows to the widest clip drawn)
  const auto n = static_cast<size_t>(cb - ca);
  if (scratch.size() < n) scratch.resize(n);
  for (size_t i = 0; i < n; ++i)
    scratch[i] = static_cast<float>(clipEnvelopeShape(env, env.bodyStart + (ca + static_cast<double>(i) + 0.5 - cx0) / k, 0.0));
  out.g = scratch.data();
  out.x0 = static_cast<int32_t>(ca);
  out.n = static_cast<uint32_t>(n);
  return out;
}

// The draw of one linear piece (clip-local px [xa, xb) showing source frames [fa, fb), mirrored when reversed) with
// the clip's left edge at x `x0` (WF.1: a double — at deep zoom a clip's left edge is far outside float's whole
// pixels, and the columns must not move by the rounding). Columns are the whole px whose centre lies in the piece.
// False: no column.
bool pieceDraw(const AudioPiece& p, double x0, float by0, float by1, float gain, Rgba col, float lo, float hi, float lineWidth,
               const ColumnEnvelope& env, WaveformDraw& d) {
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
  d.rect = {static_cast<float>(x0 + ia), by0, static_cast<float>(x0 + ib), by1};
  d.gain = gain;
  d.reverse = p.reverse;
  d.colour = col;
  d.clipX0 = lo;
  d.clipX1 = hi;
  d.lineWidth = lineWidth;
  d.envelope = env.g;
  d.envelopeX0 = env.x0;
  d.envelopeCount = env.n;
  return true;
}

// SX.4: the pieces of the clip's waveform from `src` (rendered or the mapped source).
template <class F>
void forEachWavePiece(const doc::AudioClip& c, const AudioLayout& L, const TempoMap& tm, double k, const WaveSource& src, double xFrom,
                      double xTo, F&& f) {
  if (src.rendered) forEachRenderedPiece(c, L, tm, src.rendered->rate, k, xFrom, xTo, f);
  else forEachAudioPiece(c, L, tm, k, xFrom, xTo, f);
}

}  // namespace

WaveSource clipWaveSource(Waveforms& waveforms, const doc::Snapshot& shown, doc::TrackId track, const AudioLayout& L, bool* rendering) {
  WaveSource src;
  if (rendering != nullptr) *rendering = false;
  if (L.renderKey == 0 || !L.clip || !waveforms.hasClipRenderer()) return src;
  const doc::ClipRef ref{track, L.clip->id};
  const RenderedLookup r = waveforms.rendered(shown, ref, *L.clip, L.renderKey);
  if (rendering != nullptr) *rendering = r.rendering;
  src.rendered = r.wave != nullptr && r.wave->overview ? r.wave : nullptr;
  src.doc = &shown;
  src.ref = ref;
  return src;
}

double audioSourceFrameAt(const doc::AudioClip& c, const AudioLayout& L, const TempoMap& tm, double t) {
  if (c.warp == doc::AudioWarp::Off) {
    const double t0 = std::floor(t);
    const double s0 = tm.ticksToSeconds(static_cast<Ticks>(t0)), s1 = tm.ticksToSeconds(static_cast<Ticks>(t0) + 1);
    const double x = s0 + (s1 - s0) * (t - t0) - L.startSec;
    return c.contentStartFrame + (c.reverse ? c.seconds - x : x) * L.rate * L.ratio;
  }
  const double x = t - static_cast<double>(c.start);
  const auto len = static_cast<double>(std::max<Ticks>(c.length, 1));
  double pos = static_cast<double>(c.contentStart) + (c.reverse ? len - x : x);
  const ContentLoop loop = doc::contentLoop(c);
  if (contentLooped(loop) && pos >= static_cast<double>(loop.end)) {
    const auto span = static_cast<double>(loop.end - loop.start);
    pos = static_cast<double>(loop.start) + std::fmod(pos - static_cast<double>(loop.end), span);
  }
  return doc::warpFrameAt(c.markers, L.fpt, pos);
}

namespace {

// The crossfade window of clip `c` with the clip before it in its lane (as doc::lowerAudioTrack keeps it: the neighbour
// reaches c's start and is not covered by it; at most either clip's length), centred on the boundary. False: none.
bool crossfadeWindow(const doc::AudioClip& c, const doc::AudioClip* prev, double& a, double& b) noexcept {
  if (c.crossfadeIn <= 0 || prev == nullptr || prev->start >= c.start || doc::clipEnd(*prev) < c.start) return false;
  const Ticks xf = std::min({c.crossfadeIn, std::max<Ticks>(c.length, 1), std::max<Ticks>(c.start - prev->start, 1)});
  const Ticks half = xf / 2;
  a = static_cast<double>(c.start - half);
  b = static_cast<double>(c.start + xf - half);
  return true;
}

// Whether two adjacent clips continue one source law across [a, b) (a split crossfaded back together): equal gain.
bool sameLaw(const doc::AudioClip& p, const AudioLayout& pl, const doc::AudioClip& c, const AudioLayout& cl, const TempoMap& tm,
             double a, double b) {
  if (p.sample != c.sample || p.warp != c.warp || p.reverse != c.reverse) return false;
  for (const double t : {a, 0.5 * (a + b), b})
    if (!(std::abs(audioSourceFrameAt(p, pl, tm, t) - audioSourceFrameAt(c, cl, tm, t)) <= 1e-3)) return false;
  return true;
}

}  // namespace

AudioEnvelope audioEnvelope(const doc::AudioClip& c, const AudioLayout& L, const TempoMap& tm, const AudioNeighbours& n) {
  AudioEnvelope e;
  e.gain = clipGainLinear(c.gainDb);
  const Ticks len = std::max<Ticks>(c.length, 1);
  e.bodyStart = static_cast<double>(c.start);
  e.bodyEnd = static_cast<double>(c.start + len);
  const double body = static_cast<double>(len);
  // The edge declicks in ticks (2 ms of song time at each edge).
  double dIn = 0.0, dOut = 0.0;
  if (!c.noEdgeFade) {
    const double s0 = tm.ticksToSeconds(c.start), s1 = tm.ticksToSeconds(c.start + len);
    dIn = tm.secondsToTicks(s0 + kClipEdgeDeclickSeconds) - e.bodyStart;
    dOut = e.bodyEnd - tm.secondsToTicks(std::max(s0, s1 - kClipEdgeDeclickSeconds));
  }
  const Ticks fi = std::clamp<Ticks>(c.fadeIn.length, 0, len);
  const Ticks fo = std::clamp<Ticks>(c.fadeOut.length, 0, len - fi);
  if (crossfadeWindow(c, n.prev, e.xinStart, e.xinEnd)) {
    e.inEnd = e.bodyStart;  // the crossfade replaces the fade-in and the declick
    e.xinLinear = n.prevL != nullptr && sameLaw(*n.prev, *n.prevL, c, L, tm, e.xinStart, e.xinEnd);
  } else {
    double f = static_cast<double>(fi);
    e.inCurve = std::clamp(c.fadeIn.curve, -1.0f, 1.0f);
    if (f < dIn) {
      f = dIn;
      e.inCurve = 0.0f;
    }
    e.inEnd = e.bodyStart + std::clamp(f, 0.0, body);
  }
  if (n.next != nullptr && crossfadeWindow(*n.next, &c, e.xoutStart, e.xoutEnd)) {
    e.outStart = e.bodyEnd;  // the crossfade replaces the fade-out and the declick
    e.xoutLinear = n.nextL != nullptr && sameLaw(c, L, *n.next, *n.nextL, tm, e.xoutStart, e.xoutEnd);
  } else {
    double f = static_cast<double>(fo);
    e.outCurve = std::clamp(c.fadeOut.curve, -1.0f, 1.0f);
    if (f < dOut) {
      f = dOut;
      e.outCurve = 0.0f;
    }
    e.outStart = e.bodyEnd - std::clamp(f, 0.0, body);
  }
  return e;
}

AudioEnvelope laneAudioEnvelope(Model& m, const ArrangementRow& row, std::span<const doc::ClipSlot> lane, size_t index,
                                const TempoMap& tm) {
  if (index >= lane.size() || !lane[index].audio) return {};
  const doc::AudioClipPtr& c = lane[index].audio;
  AudioNeighbours n;
  if (index > 0 && lane[index - 1].audio && c->crossfadeIn > 0) {
    n.prev = lane[index - 1].audio.get();
    n.prevL = &m.audioLayout(row, lane[index - 1].audio);
  }
  if (index + 1 < lane.size() && lane[index + 1].audio && lane[index + 1].audio->crossfadeIn > 0) {
    n.next = lane[index + 1].audio.get();
    n.nextL = &m.audioLayout(row, lane[index + 1].audio);
  }
  return audioEnvelope(*c, m.audioLayout(row, c), tm, n);
}

// WF.1: one direct draw of a clip's waveform for every view that shows one (the arrangement's lanes, the project patch
// view's lane windows): see internal.h.
ClipWaveStats paintClipWaveform(ui::DrawList& dl, Waveforms& waveforms, const media::WaveformOverview& ov, const doc::AudioClip& c,
                                const AudioLayout& L, const AudioEnvelope& env, const TempoMap& tm, double k, double cx0, float vx0,
                                float vx1, float by0, float by1, ui::Rgba col, float lineWidth, const WaveSource& src) {
  ClipWaveStats out;
  const media::WaveformOverview& o = src.rendered ? *src.rendered->overview : ov;
  WaveformLevel lv[kMaxOverviewLevels];
  WaveformData data = waveformData(o, nullptr, lv);
  const double lo = static_cast<double>(vx0) - cx0, hi = static_cast<double>(vx1) - cx0;
  if (waveformLevelFor(data, framesPerPx(L, c, tm, k, src)) < 0) {  // below the overview: the samples under the columns
    if (src.rendered) {  // SX.4: the render's own frames (all of them, or a re-render when they were dropped)
      if (src.doc != nullptr)
        if (const media::SampleWindow* w = waveforms.renderedFrames(*src.doc, src.ref, c, *src.rendered)) data = waveformData(o, w, lv);
    } else {
      double f0 = 1e300, f1 = -1e300;
      forEachAudioPiece(c, L, tm, k, lo, hi, [&](const AudioPiece& p) {
        f0 = std::min(f0, p.fa);
        f1 = std::max(f1, p.fb);
      });
      if (f1 > f0)
        if (const media::SampleWindow* w = waveforms.window(c.sample, static_cast<int64_t>(std::floor(f0)) - ui::kWaveformSampleMargin,
                                                           static_cast<int64_t>(std::ceil(f1)) + ui::kWaveformSampleMargin))
          data = waveformData(o, w, lv);
    }
  }
  // SX.3: the envelope over the visible columns (one neighbour either side), applied by drawWaveform per column.
  const double ca = std::floor(std::max(static_cast<double>(vx0), cx0)) - 1.0;
  const double cb = std::ceil(std::min(static_cast<double>(vx1), cx0 + static_cast<double>(c.length) * k)) + 1.0;
  const ColumnEnvelope ce = columnEnvelope(env, cx0, k, ca, cb);
  forEachWavePiece(c, L, tm, k, src, lo, hi, [&](const AudioPiece& p) {
    WaveformDraw d;
    if (!pieceDraw(p, cx0, by0, by1, env.gain, col, vx0, vx1, lineWidth, ce, d)) return;
    const WaveformStats st = drawWaveform(dl, data, d);
    out.columns += st.columns;
    out.approx = out.approx || st.approx;
    out.samples = out.samples || st.level == -1;
    out.curve = out.curve || st.curve;
    out.dots += st.dots;
  });
  return out;
}

namespace {

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
bool buildAudioTile(TileAtlas& tiles, AudioLayout& L, const AudioEnvelope& env, const media::WaveformOverview& ov, double k,
                    int32_t bodyH, Model& m, float lineWidth, const WaveSource& src) {
  const doc::AudioClip& c = *L.clip;
  AudioTile& t = L.tile;
  const int32_t width = localWidth(c.length, k);
  t = AudioTile{};
  t.k = k;
  t.width = width;
  t.bodyH = bodyH;
  t.built = ov.builtFrames();
  t.env = env;
  t.src = &ov;
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
  // The strips' coverage written directly (ui::rasterWaveform: the pixels drawWaveform covers, without triangles),
  // SX.3: through the envelope (the tile is keyed by it).
  const ColumnEnvelope ce = columnEnvelope(env, 0.0, k, -1.0, static_cast<double>(width) + 1.0);
  forEachWavePiece(c, L, *L.tempo, k, src, 0.0, static_cast<double>(width), [&](const AudioPiece& p) {
    WaveformDraw d;
    if (pieceDraw(p, 0.0, 0.0f, static_cast<float>(bodyH), env.gain, rgba(255, 255, 255), -3e38f, 3e38f, lineWidth, ce, d))
      (void)rasterWaveform(data, d, dst + pitch + 1, static_cast<uint32_t>(width), static_cast<uint32_t>(bodyH), pitch);
  });
  t.tile = tile;
  t.state = PreviewTile::State::Ready;
  ++m.stats.tilesBuilt;
  return true;
}

// SX.3: the processing tag of a clip (none for a clip that plays its file 1:1: Off, no transpose / detune / reverse): the
// mode in capitals ("WARP · Beats", "RE-PITCH") and the details beside it ("+3 st", "-12 c", "Rev"), in the mode's
// colour (time-stretch amber, Re-Pitch teal) on a dark pill (legible on every clip colour); an Off clip resampled by its
// transpose or reversed: the details alone ("+7 st  Rev", "REV") in violet.
struct ClipTag {
  std::string mode, detail;
  Rgba colour = 0;
  bool shown() const noexcept { return !mode.empty(); }
};

constexpr Rgba kTagPill = rgba(18, 18, 22, 225);

ClipTag clipTag(const doc::AudioClip& c) {
  ClipTag t;
  char buf[48];
  auto add = [&](const char* x) {
    if (!t.detail.empty()) t.detail += "  ";
    t.detail += x;
  };
  if (doc::warpTransposes(c.warp)) {  // Off and the time-stretch modes play the transpose
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
  }
  const bool pitched = !t.detail.empty();
  if (c.reverse) add("Rev");
  if (dsp::warpStretches(c.warp)) {
    static constexpr const char* kMode[] = {"", "", "Beats", "Tones", "Texture", "Complex", "Pro"};
    t.mode = std::string("WARP \xC2\xB7 ") + kMode[static_cast<size_t>(c.warp)];
    t.colour = rgba(255, 184, 72);
  } else if (c.warp == doc::AudioWarp::RePitch) {
    t.mode = "RE-PITCH";
    t.colour = rgba(96, 216, 232);
  } else if (pitched) {
    t.mode = std::move(t.detail);
    t.detail.clear();
    t.colour = rgba(200, 168, 255);
  } else if (c.reverse) {
    t.mode = "REV";
    t.detail.clear();
    t.colour = rgba(200, 168, 255);
  }
  return t;
}

// The tag, right-aligned in the title strip (at the clip's or the canvas's right edge, whichever is nearer) where it fits
// beside the start of the name (else its mode alone); else (a short clip) a corner mark in the tag's colour at the title
// strip's right end.
void paintClipTag(LanePaint& lp, const ClipCols& cc, float top, float titleB, const ClipTag& tag) {
  if (!tag.shown() || titleB - top < 4.0f) return;
  DrawList& dl = lp.ctx.dl;
  const float right = std::min(cc.cx1, static_cast<float>(lp.r.right()));
  const float h = titleB - top;
  const float pad = static_cast<float>(lp.ctx.scale.px(4.0f));
  if (lp.font != kNoFont && lp.ctx.res.text) {
    TextSystem& ts = *lp.ctx.res.text;
    for (const bool full : {true, false}) {
      if (full == tag.detail.empty()) continue;  // (no details: the mode once)
      const TextRun run = ts.shape(lp.font, full ? tag.mode + "  " + tag.detail : tag.mode);
      const float w = static_cast<float>(run.advance) + 2.0f * pad;
      const float x0 = std::floor(right - 2.0f - w);
      // BR.J (bug 2026-11-27_18-01-03): the layout depends on the clip and the canvas only, never on the columns being
      // repainted (lp.c): it tested the pill's end against them, so the playhead's damage strips drew the short tag or
      // the corner mark where a full repaint drew the whole tag, and pieces of each stayed behind the playhead.
      if (x0 >= cc.cx0 + static_cast<float>(lp.ctx.scale.px(40.0f)) && x0 >= static_cast<float>(lp.r.x)) {
        const Rect pill{x0, top + 1.0f, x0 + w, titleB - 1.0f};
        dl.addRoundedRectFilled(pill, std::min(3.0f * lp.ctx.scale.factor, 0.5f * (pill.y1 - pill.y0)), kTagPill);
        TextSystem::drawRun(dl, run, {x0 + pad, top + static_cast<float>(lp.baselineOff)}, tag.colour);
        return;
      }
    }
  }
  // The corner mark: a right triangle in the strip's top-right corner, the tag's colour inside a dark one.
  const float m = std::min({h, std::round(0.75f * h), cc.cx1 - cc.cx0 - 1.0f});
  if (m < 3.0f || right <= cc.cx0 + m) return;
  dl.addTriangleFilled({right - m, top}, {right, top}, {right, top + m}, kTagPill);
  const float in = std::max(1.0f, std::round(0.2f * m));
  dl.addTriangleFilled({right - m + 2.4f * in, top + in}, {right - in, top + in}, {right - in, top + m - 2.4f * in}, tag.colour);
}

// Fade curves (fade-in from the left edge, fade-out to the right edge; SX.3: the waveform itself is faded, so no shading),
// the crossfade X and the handles, into `o` (the overlay list).
// A clip whose file is missing: a dark body with light diagonal hatching (it reads as "nothing here" at any width) and
// "Missing" in white, centred, where it fits (M5 review UI 1: the red badge on the title strip was 1.39:1).
void paintMissingBody(LanePaint& lp, const ClipCols& cc, float by0, float by1) {
  DrawList& dl = lp.ctx.dl;
  dl.addRectFilled({cc.vx0, by0, cc.vx1, by1}, rgba(36, 36, 38));
  const float step = static_cast<float>(std::max(4, lp.ctx.scale.px(7.0f)));
  const float h = by1 - by0;
  const Rgba hatch = rgba(255, 255, 255, 34);
  const float lw = std::max(1.0f, std::round(lp.ctx.scale.factor));
  dl.pushClip({cc.vx0, by0, cc.vx1, by1});
  // Stripes rising to the right, anchored to the clip (they scroll with it).
  const float first = cc.cx0 + std::floor((cc.vx0 - h - cc.cx0) / step) * step;
  for (float x = first; x < cc.vx1; x += step) {
    const Vec2 seg[2] = {{x, by1}, {x + h, by0}};
    smoothPolyline(dl, seg, 2, hatch, lw);
  }
  dl.popClip();
  if (lp.font != kNoFont && lp.ctx.res.text && h >= static_cast<float>(lp.ctx.scale.px(12.0f))) {
    TextSystem& ts = *lp.ctx.res.text;
    const TextRun run = ts.shape(lp.font, "Missing");
    // BR.J: centred in the clip's part of the canvas (lp.r), not of the columns being repainted (cc.vx*: the playhead's
    // damage strips drew the label at their own centres).
    const float vis0 = std::max(cc.cx0, static_cast<float>(lp.r.x)), vis1 = std::min(cc.cx1, static_cast<float>(lp.r.right()));
    const float pad = static_cast<float>(lp.ctx.scale.px(4.0f));
    if (vis1 - vis0 >= static_cast<float>(run.advance) + 2.0f * pad) {
      const float x = std::floor(0.5f * (vis0 + vis1 - static_cast<float>(run.advance)));
      const float y = std::floor(0.5f * (by0 + by1) + static_cast<float>(lp.baselineOff) - 0.5f * static_cast<float>(lp.titleH));
      dl.addRectFilled({x - pad, y - static_cast<float>(lp.baselineOff) + 1.0f, x + static_cast<float>(run.advance) + pad,
                        y - static_cast<float>(lp.baselineOff) + static_cast<float>(lp.titleH) - 1.0f},
                       rgba(36, 36, 38));
      TextSystem::drawRun(dl, run, {x, y}, rgba(240, 240, 240));
    }
  }
}

// M5 review UI 3: fade curves and the crossfade X in a dark shade of the clip colour (they were opaque white over the
// waveform); the X only while the clip is selected or hovered (the window is shaded lightly otherwise, so a crossfade
// stays visible). SX.3: the fades' darkening went (the waveform shows the fade).
Rgba fadeLineColour(Rgba clip) { return withAlpha(lerp(clip, rgba(0, 0, 0), 0.62f), 235); }

void crossfadeMarks(DrawList& o, float xa, float xb, float by0, float by1, Rgba line, float lw, bool x) {
  o.addRectFilled({xa, by0, xb, by1}, rgba(0, 0, 0, 36));
  if (!x) return;
  const Vec2 a[2] = {{xa, by0}, {xb, by1}}, b[2] = {{xa, by1}, {xb, by0}};
  smoothPolyline(o, a, 2, line, lw);
  smoothPolyline(o, b, 2, line, lw);
}

// SX.4: a clip whose rendered waveform is on its way: a dashed line in the warp tag's amber along the bottom of its body
// (subtle; gone with the swap).
void paintRenderingMark(DrawList& o, LanePaint& lp, const ClipCols& cc, float by1) {
  const float h = std::max(1.0f, std::round(1.5f * lp.ctx.scale.factor));
  const float dash = static_cast<float>(lp.ctx.scale.px(6.0f)), gap = static_cast<float>(lp.ctx.scale.px(4.0f));
  const float x0 = std::max(cc.vx0, cc.cx0), x1 = std::min(cc.vx1, cc.cx1);
  const Rgba col = rgba(255, 184, 72, 170);
  for (float x = cc.cx0 + std::floor((x0 - cc.cx0) / (dash + gap)) * (dash + gap); x < x1; x += dash + gap) {
    const float a = std::max(x, x0), b = std::min(x + dash, x1);
    if (b > a) o.addRectFilled({a, by1 - h, b, by1}, col);
  }
}

void paintOverlays(DrawList& o, LanePaint& lp, const doc::AudioClip& c, const AudioEnvelope& env, const ClipCols& cc, float by0,
                   float by1, bool handles, int hover, Rgba colour) {
  const float scale = lp.ctx.scale.factor;
  const Rgba line = fadeLineColour(colour);
  const float lw = std::max(1.0f, std::round(scale));
  const AudioFadesPx f = audioFadesPx(c, lp.k);
  Vec2 pts[65];
  auto fade = [&](float x0, float w, float curve, bool in) {
    if (w < 1.0f) return;
    const int n = std::clamp(static_cast<int>(w / 3.0f), 2, 64);
    for (int i = 0; i <= n; ++i) {
      const double x = static_cast<double>(i) / n;
      const double g = in ? clipFadeInShape(x, curve) : clipFadeOutShape(x, curve);
      pts[i] = {x0 + w * static_cast<float>(x), by1 - static_cast<float>(g) * (by1 - by0)};
    }
    smoothPolyline(o, pts, static_cast<uint32_t>(n + 1), line, lw);
  };
  if (c.crossfadeIn == 0) fade(cc.cx0, f.in, c.fadeIn.curve, true);
  if (!(env.xoutEnd > env.xoutStart)) fade(cc.cx1 - f.out, f.out, c.fadeOut.curve, false);  // (a crossfade out replaces it)
  if (c.crossfadeIn > 0) {  // centred on the boundary (both clips' parts of the window)
    const float xa = cc.cx0 - static_cast<float>(static_cast<double>(c.crossfadeIn / 2) * lp.k);
    const float xb = cc.cx0 + static_cast<float>(static_cast<double>(c.crossfadeIn - c.crossfadeIn / 2) * lp.k);
    crossfadeMarks(o, xa, xb, by0, by1, line, lw, handles);
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
                    float bottom, bool trackActive, bool selected, int hover, std::span<const doc::ClipSlot> lane, size_t index) {
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

  if (available) paintClipTag(lp, cc, top, titleB, clipTag(own));

  const float by0 = titleB, by1 = bottom - 1.0f;
  const int32_t bodyH = static_cast<int32_t>(by1 - by0);
  if (!available) {  // the clip plays silence: a dark hatched body with "Missing" (M5 review UI 1)
    if (by1 > by0) paintMissingBody(lp, cc, by0, by1);
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
  // SX.3: its envelope (a drag preview: its own fades, no crossfade); while a fade or gain handle is dragged no tile is
  // built (the dragged clip and its crossfade partner draw directly, the envelope applied per column; one tile on
  // release).
  const bool inLane = ownContent && index < lane.size() && lane[index].audio == clipPtr;
  const AudioEnvelope env = inLane ? laneAudioEnvelope(lp.m, row, lane, index, *L->tempo) : audioEnvelope(c, *L, *L->tempo, {});
  const DragKind dk = lp.arrangementLanes ? lp.view.editor().dragKind() : DragKind::None;
  const bool envelopeDrag = dk == DragKind::Fade || dk == DragKind::Gain;
  const Rgba waveCol = clipNoteColour(colour);
  bool deferred = false;
  const media::WaveformOverview* srcOv = ctx.waveforms().overview(own.sample);
  // SX.4: a time-stretch clip from its rendered audio (a drag preview: its source, mapped); while it renders, the mapped
  // source (or its previous render) with the rendering mark.
  bool rendering = false;
  const WaveSource src = ownContent ? clipWaveSource(ctx.waveforms(), lp.m.shown, row.id, base, &rendering) : WaveSource{};
  const media::WaveformOverview* ov = src.rendered ? src.rendered->overview.get() : srcOv;
  const bool detailed = cc.cx1 - cc.cx0 >= static_cast<float>(lp.ctx.scale.px(4.0f)) && bodyH >= 3;
  if (ov && detailed) {
    TileAtlas* tiles = lp.tiles();
    const double fpp = framesPerPx(*L, c, *L->tempo, lp.k, src);
    WaveformLevel lv[kMaxOverviewLevels];
    WaveformData data = waveformData(*ov, nullptr, lv);
    const bool sampleLevel = waveformLevelFor(data, fpp) < 0;
    const int32_t width = localWidth(c.length, lp.k);
    bool direct = true;
    if (tiles && ownContent && !sampleLevel && wantsAudioTile(*tiles, width, bodyH)) {
      AudioTile& t = base.tile;
      const bool ready = t.state == PreviewTile::State::Ready && tiles->valid(t.tile);
      if (t.matches(lp.k, bodyH, ov->builtFrames(), env, ov) && ready) {
        queueAudioTile(lp, *tiles, t, cc.cx0, cc.vx0, cc.vx1, by0, bodyH, waveCol);
        tiles->touch(t.tile.page);
        ++lp.m.stats.tilesDrawn;
        deferred = true;
        direct = false;
      } else if (!(t.matches(lp.k, bodyH, ov->builtFrames(), env, ov) && t.state == PreviewTile::State::Failed)) {
        if (!envelopeDrag) {
          ++lp.m.stats.tilesMissing;
          lp.m.tilesMissing = true;
          if (lp.zooming()) lp.m.tilesAfterZoom = true;
        }
        if (ready && t.bodyH == bodyH && t.env == env) {  // the previous zoom's tile, stretched, until onFrame has built this one
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
      // WF.1: the clip's left edge in double (cc.cx0 is a float: whole pixels only up to 2^24).
      const double cx0 = std::round(static_cast<double>(lp.r.x) + (static_cast<double>(g.start) - lp.origin) * lp.k);
      const ClipWaveStats st = paintClipWaveform(dl, ctx.waveforms(), *ov, c, *L, env, *L->tempo, lp.k, cx0, cc.vx0, cc.vx1, by0, by1,
                                                 waveCol, std::max(1.0f, lp.ctx.scale.factor), src);
      lp.m.stats.waveColumns += st.columns;
      lp.m.stats.waveSampleDraws += st.samples ? 1 : 0;
      lp.m.stats.waveCurveDraws += st.curve ? 1 : 0;
      lp.m.stats.waveApprox += st.approx ? 1 : 0;
      lp.m.stats.waveDots += st.dots;
      ++lp.m.stats.wavesDirect;
    }
  }
  paintClipBorder(lp, cc, top, bottom, titleB, selected, hover >= -1 && hover <= 1 ? hover : kNoHover, deferred);
  const bool handles = selected || hover != kNoHover;
  paintOverlays(lp.m.overlay, lp, c, env, cc, by0, by1, handles, hover, colour);
  if (rendering && detailed) paintRenderingMark(lp.m.overlay, lp, cc, by1);
}

void paintAudioCrossfade(LanePaint& lp, const doc::AudioClip& c, float top, float bottom, bool marked) {
  if (c.crossfadeIn <= 0 || !lp.view.context().sampleAvailable(c.sample)) return;
  ClipCols cc;
  cc.cx0 = std::round(lp.X(static_cast<double>(c.start)));
  cc.cx1 = std::max(cc.cx0 + 1.0f, std::round(lp.X(static_cast<double>(c.start + c.length))));
  const float by0 = std::min(bottom, top + static_cast<float>(lp.titleH)), by1 = bottom - 1.0f;
  const float xa = cc.cx0 - static_cast<float>(static_cast<double>(c.crossfadeIn / 2) * lp.k);
  const float xb = cc.cx0 + static_cast<float>(static_cast<double>(c.crossfadeIn - c.crossfadeIn / 2) * lp.k);
  if (xb < static_cast<float>(lp.c.x) - 1.0f || xa > static_cast<float>(lp.c.right()) + 1.0f || by1 <= by0) return;
  const float lw = std::max(1.0f, std::round(lp.ctx.scale.factor));
  crossfadeMarks(lp.m.overlay, xa, xb, by0, by1, fadeLineColour(rgbOf(c.colour)), lw, marked);
}

bool buildAudioTiles(LaneCanvas& canvas, ArrangementView& view, TileAtlas& tiles, const IRect& visible, size_t& texels, bool& builtOne) {
  Model& m = view.model();
  const IRect r = canvas.rect();
  const IRect c = intersect(r, visible);
  if (c.empty() || !m.shown) return true;
  // SX.3: none while a fade or gain handle is dragged (those clips draw directly; the release repaints and builds).
  if (const DragKind dk = view.editor().dragKind(); dk == DragKind::Fade || dk == DragKind::Gain) return true;
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
  // The track rows' clips, then (T6.10) the take lanes' (strip: the row, its clips, their index, the lane's height).
  using Strip = Model::AudioStrip;
  std::vector<Strip>& strips = m.audioStrips;
  strips.clear();
  for (auto it = first; it != rows.end() && it->y < localBottom; ++it)
    if (!it->group && it->track->kind == doc::TrackKind::Audio && !it->track->clips.empty())
      strips.push_back({&*it, &it->track->clips, &m.clips(*it->track), it->h});
  const auto takes = view.takeRows();
  for (auto it = std::upper_bound(takes.begin(), takes.end(), localTop, [](int32_t y, const TakeLaneRow& r) { return y < r.y + r.h; });
       it != takes.end() && it->y < localBottom; ++it)
    if (rows[static_cast<size_t>(it->row)].track->kind == doc::TrackKind::Audio && !it->take->clips.empty())
      strips.push_back({&rows[static_cast<size_t>(it->row)], &it->take->clips, &m.clips(*it->take), it->h});
  for (const Strip& st : strips) {
    const ArrangementRow* it = st.row;
    // The body as paintAudioClip computes it: [top + titleH, bottom − 1) of the clip's [y + 1, y + h − 1).
    const int32_t bodyH = st.h - 3 - titleH;
    if (bodyH < 3) continue;
    const ClipIndex& ix = *st.ix;
    const auto [a, b] = ix.visible(from, to);
    for (size_t j = a; j < b; ++j) {
      const doc::AudioClipPtr& clip = (*st.clips)[j].audio;
      if (!clip || !ctx.sampleAvailable(clip->sample)) continue;
      const media::WaveformOverview* srcOv = ctx.waveforms().overview(clip->sample);
      if (!srcOv) continue;
      AudioLayout& L = m.audioLayout(*it, clip);
      const int32_t width = localWidth(clip->length, k);
      if (!wantsAudioTile(tiles, width, bodyH)) continue;
      const WaveSource src = clipWaveSource(ctx.waveforms(), m.shown, it->id, L, nullptr);  // SX.4
      const media::WaveformOverview* ov = src.rendered ? src.rendered->overview.get() : srcOv;
      WaveformLevel lv[kMaxOverviewLevels];
      if (waveformLevelFor(waveformData(*ov, nullptr, lv), framesPerPx(L, *clip, *L.tempo, k, src)) < 0) continue;
      const AudioEnvelope env = laneAudioEnvelope(m, *it, *st.clips, j, *L.tempo);
      const AudioTile& at = L.tile;
      if (at.matches(k, bodyH, ov->builtFrames(), env, ov) &&
          (at.state == PreviewTile::State::Failed || (at.state == PreviewTile::State::Ready && tiles.valid(at.tile)))) {
        if (at.state == PreviewTile::State::Ready) tiles.touch(at.tile.page);
        continue;
      }
      const size_t area = static_cast<size_t>(width + 2) * static_cast<size_t>(bodyH + 2);
      if (builtOne && texels + area > am::kTileBuildTexelsPerFrame) return false;
      (void)buildAudioTile(tiles, L, env, *ov, k, bodyH, m, std::max(1.0f, s.factor), src);
      texels += area;
      builtOne = true;
    }
  }
  return true;
}

}  // namespace pq::views::arrangement
