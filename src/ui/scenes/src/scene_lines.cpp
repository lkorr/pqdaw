// The T0.9 acceptance scene: PQ Meter's line primitives on deterministic signals.
//   top-left   oscilloscope: a sine at PQ Meter's defaults (line 1.4 px, glow 0.35) through splinePath, plus a
//              coarsely sampled sine (one point per 24 px) whose smoothness is all Catmull-Rom, with an
//              additive glow
//   top-right  envelope: dense noise drawn as a min/max band (scopeEnvelope), and a per-vertex-coloured
//              zig-zag with spikes sharper than 120° (smoothPolylineCols bevels)
//   middle     spectrum: a noise spectrum with PQ Meter's layering (gradient fill, 3-pass level-weighted bloom,
//              body, bright core) coloured through a Gradient (Ice), and a splineSmooth'ed peak-hold line
//   bottom     the ten colormaps, and a PaletteTrack strip blending Magma → Ice → Viridis
// Positions scale with the target (800×480 design space); line widths stay in pixels.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "pq/ui/colormap.h"
#include "pq/ui/lines.h"
#include "scenes_internal.h"

namespace pq::ui::detail {

namespace {

// xorshift32: deterministic noise for the scene (the golden image must not depend on a library RNG).
struct Rng {
  uint32_t s = 0x9E3779B9u;
  float next() noexcept {  // uniform in [-1, 1)
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
  }
};

void panel(DrawList& dl, const Rect& r) {
  using namespace signal_theme;
  dl.addRectFilled(r, kPanel);
  // Grid: quarters both ways, 1 px lines at the grid token.
  for (int i = 1; i < 4; ++i) {
    const float x = std::round(r.x0 + r.width() * static_cast<float>(i) / 4.0f);
    const float y = std::round(r.y0 + r.height() * static_cast<float>(i) / 4.0f);
    dl.addRectFilled({x, r.y0, x + 1, r.y1}, kGrid);
    dl.addRectFilled({r.x0, y, r.x1, y + 1}, kGrid);
  }
}

}  // namespace

void linesScene(DrawList& dl, const SceneTextures& /*tex*/, double time) {
  using namespace signal_theme;
  const float W = static_cast<float>(dl.targetWidth()), H = static_cast<float>(dl.targetHeight());
  const float sx = W / 800.0f, sy = H / 480.0f;
  auto R = [&](float x0, float y0, float x1, float y1) {
    return Rect{std::round(x0 * sx), std::round(y0 * sy), std::round(x1 * sx), std::round(y1 * sy)};
  };
  const float t = static_cast<float>(time);
  constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

  // --- oscilloscope ---------------------------------------------------------------------------------------
  {
    const Rect r = R(16, 16, 392, 200);
    panel(dl, r);
    dl.pushClip(r);
    const float cy = 0.5f * (r.y0 + r.y1), amp = 0.36f * r.height();
    // Half-pixel sample spacing, as the scope's interpolated trace arrives.
    std::vector<Vec2> pts;
    for (float x = r.x0; x <= r.x1; x += 0.5f) {
      const float ph = (x - r.x0) / r.width() * 2.25f * kTau + t;
      pts.push_back({x, cy - amp * (std::sin(ph) + 0.18f * std::sin(3.0f * ph))});
    }
    std::vector<Vec2> smoothed;
    splinePath(pts, smoothed, 1.25f);
    scopeTrace(dl, smoothed, kWaveform, 1.4f, 0.35f);
    // Coarse: one point per 24 px. The curve between them is Catmull-Rom subdivision at ≤ 1.5 px chords.
    std::vector<Vec2> coarse;
    const int nCoarse = static_cast<int>(std::ceil(r.width() / (24.0f * sx))) + 1;
    for (int i = 0; i < nCoarse; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(nCoarse - 1);
      const float ph = u * 1.5f * kTau + 1.3f + 0.6f * t;
      coarse.push_back({r.x0 + u * r.width(), cy + 0.55f * amp * std::sin(ph)});
    }
    splinePath(coarse, smoothed, 1.5f);
    scopeTrace(dl, smoothed, kWaveformR, 1.0f, 0.6f, BlendMode::Additive);
    // The control points, so the subdivision is visible.
    for (const Vec2& p : coarse)
      dl.addRoundedRectFilled({p.x - 2.5f, p.y - 2.5f, p.x + 2.5f, p.y + 2.5f}, 2.5f, withAlpha(kWaveformR, 200));
    dl.popClip();
  }

  // --- envelope + bevelled zig-zag ----------------------------------------------------------------------
  {
    const Rect r = R(408, 16, 784, 200);
    panel(dl, r);
    dl.pushClip(r);
    Rng rng;
    const float top = r.y0 + 0.30f * r.height(), amp = 0.22f * r.height();
    // One column per pixel: min/max of 64 "samples" of a decaying noise burst riding on a slow wobble.
    const uint32_t cols = static_cast<uint32_t>(r.width());
    std::vector<Vec2> up(cols), dn(cols);
    for (uint32_t c = 0; c < cols; ++c) {
      const float u = static_cast<float>(c) / static_cast<float>(cols);
      const float env = 0.08f + 0.92f * std::exp(-3.5f * u);
      const float wobble = 0.45f * env * std::sin(u * 31.0f + t) + 0.12f * std::sin(u * 7.0f);
      const float spread = 0.55f * env * (0.5f + 0.5f * std::fabs(std::sin(u * 53.0f)));
      float lo = 1.0f, hi = -1.0f;
      for (int k = 0; k < 64; ++k) {
        const float v = wobble + spread * rng.next();
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      float yHi = top - amp * hi, yLo = top - amp * lo;
      if (yLo - yHi < 0.5f) {  // hold a flat column open, as PQ Meter does
        const float mid = 0.5f * (yHi + yLo);
        yHi = mid - 0.25f;
        yLo = mid + 0.25f;
      }
      const float x = r.x0 + static_cast<float>(c) + 0.5f;
      up[c] = {x, yHi};
      dn[c] = {x, yLo};
    }
    scopeEnvelope(dl, up.data(), dn.data(), cols, withAlpha(kAccent, 200), 1.4f, 0.35f);

    // Zig-zag with alternating spike heights: turns sharper than 120° get bevels, gentler ones miters.
    const ColorLut& lut = colormapLut(Colormap::Neon);
    std::vector<Vec2> zz;
    std::vector<Rgba> zc;
    const float base = r.y0 + 0.82f * r.height();
    for (int i = 0; i <= 24; ++i) {
      const float x = r.x0 + 12.0f * sx + static_cast<float>(i) * (r.width() - 24.0f * sx) / 24.0f;
      const float h = (i % 2 ? 1.0f : -1.0f) * (i % 4 < 2 ? 0.16f : 0.05f) * r.height();
      zz.push_back({x, base + h});
      zc.push_back(lutAt(lut, 0.35f + 0.65f * static_cast<float>(i) / 24.0f));
    }
    smoothPolylineCols(dl, zz.data(), zc.data(), static_cast<uint32_t>(zz.size()), 1.6f);
    dl.popClip();
  }

  // --- spectrum ---------------------------------------------------------------------------------------------
  {
    const Rect r = R(16, 216, 784, 400);
    dl.addRectFilled(r, kPanel);
    dl.pushClip(r);
    constexpr float floorDb = -78.0f, ceilDb = 0.0f;
    const float invRange = 1.0f / (ceilDb - floorDb);
    auto dbToY = [&](float db) { return r.y0 + (ceilDb - std::clamp(db, floorDb, ceilDb)) * invRange * r.height(); };
    // PQ Meter's grid: 12 dB rules, and decade lines on the log frequency axis (20 Hz .. 20 kHz).
    for (float db = -72.0f; db < 0.0f; db += 12.0f) {
      const float y = std::round(dbToY(db));
      dl.addRectFilled({r.x0, y, r.x1, y + 1}, kGrid);
    }
    for (float f : {100.0f, 1000.0f, 10000.0f}) {
      const float x = std::round(r.x0 + std::log10(f / 20.0f) / 3.0f * r.width());
      dl.addRectFilled({x, r.y0, x + 1, r.y1}, kGrid);
    }
    const uint32_t steps = std::min(1024u, std::max(32u, static_cast<uint32_t>(r.width())));
    Rng rng;
    std::vector<Vec2> poly(steps), peak(steps);
    std::vector<Rgba> cols(steps), pkc(steps);
    std::vector<float> lvl(steps), pdbRaw(steps), pdbSm(steps), levelDb(steps);
    Gradient g;
    g.seed(Colormap::Ice);
    g.setStop(4, {0.85f, 0.95f, 1.0f}, 0.86f);  // a custom stop, as the Gradient editor would set
    float noise = 0.0f;
    for (uint32_t s = 0; s < steps; ++s) {
      const float u = static_cast<float>(s) / static_cast<float>(steps - 1);
      const float f = 20.0f * std::pow(1000.0f, u);  // 20 Hz .. 20 kHz on a log axis
      // Pink-ish tilt, a harmonic series on 110 Hz, a resonant bump, and noise smoothed along the axis (the
      // analyser's own smoothing).
      float db = -44.0f - 3.0f * std::log2(f / 1000.0f) - 6.0f * u;
      for (int h = 1; h <= 12; ++h) {
        const float d = std::log2(f / (110.0f * static_cast<float>(h))) * 40.0f;
        db += (30.0f - 1.8f * static_cast<float>(h)) * std::exp(-d * d);
      }
      const float bump = std::log2(f / 2500.0f) * 2.0f;
      noise = 0.6f * noise + 0.4f * rng.next();
      db += 9.0f * std::exp(-bump * bump) + 4.0f * noise + 1.5f * std::sin(t * 1.3f + u * 20.0f);
      if (f > 12000.0f) db -= (f - 12000.0f) * 0.006f;  // a lowpass roll-off into the floor (fades out)
      levelDb[s] = db;
      const float lt = std::clamp((db - floorDb) * invRange, 0.0f, 1.0f);
      const float vis = std::clamp((db - floorDb) / std::max(1.0f, (ceilDb - floorDb) * 0.04f), 0.0f, 1.0f);
      poly[s] = {r.x0 + u * r.width(), dbToY(db)};
      lvl[s] = lt;
      cols[s] = mulAlpha(lutAt(g.lut, 0.25f + 0.75f * lt), vis);
    }
    // Held peaks: bucket maxima a few dB over the curve, i.e. flat shelves with hard steps between them (the
    // staircase splineSmooth exists to remove).
    for (uint32_t s = 0; s < steps; ++s) {
      const uint32_t b0 = s / 16 * 16, b1 = std::min(steps, b0 + 16);
      float m = -200.0f;
      for (uint32_t k = b0; k < b1; ++k) m = std::max(m, levelDb[k] + 4.0f);
      pdbRaw[s] = m;
    }
    SpectrumCurveStyle st;
    spectrumCurve(dl, poly.data(), cols.data(), lvl.data(), steps, r.y1, st);
    splineSmooth(pdbRaw.data(), pdbSm.data(), steps, 8);
    bool anyPeak = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float u = static_cast<float>(s) / static_cast<float>(steps - 1);
      const float db = pdbSm[s];
      peak[s] = {r.x0 + u * r.width(), dbToY(db)};
      const float lead = std::clamp((db - levelDb[s]) / 6.0f, 0.0f, 1.0f);
      const float pv = std::clamp((db - floorDb) / std::max(1.0f, (ceilDb - floorDb) * 0.04f), 0.0f, 1.0f);
      if (pv > 0.0f) anyPeak = true;
      const float pa = pv * (70.0f + 150.0f * lead) / 255.0f;
      pkc[s] = rgba(232, 240, 255, static_cast<uint32_t>(pa * 255.0f + 0.5f));
    }
    if (anyPeak) gradientPolyline(dl, peak.data(), pkc.data(), steps, 1.3f);
    dl.popClip();
  }

  // --- colormaps + palette track -----------------------------------------------------------------------------
  {
    const float y0 = std::round(416 * sy), y1 = std::round(464 * sy);
    const float stripW = (500.0f - 9.0f * 4.0f) / 10.0f;
    for (int m = 0; m < kColormapCount; ++m) {
      const float x0 = (16.0f + static_cast<float>(m) * (stripW + 4.0f)) * sx;
      const ColorLut& lut = colormapLut(static_cast<Colormap>(m));
      // Low at the bottom, high at the top, as meters paint through them.
      constexpr int kRows = 16;
      for (int b = 0; b < kRows; ++b) {
        const float vt = 1.0f - static_cast<float>(b) / kRows, vb = 1.0f - static_cast<float>(b + 1) / kRows;
        const float ya = y0 + (y1 - y0) * static_cast<float>(b) / kRows;
        const float yb = y0 + (y1 - y0) * static_cast<float>(b + 1) / kRows;
        const Rgba ct = lutAt(lut, vt), cb = lutAt(lut, vb);
        dl.addRectFilledMultiColor({std::round(x0), ya, std::round(x0 + stripW * sx), yb}, ct, ct, cb, cb);
      }
    }
    PaletteTrack track;
    track.setEnabled(true);
    const int mid = track.insertWaypoint();
    track.setWaypointPreset(mid, Colormap::Ice);
    const float tx0 = std::round(528 * sx), tx1 = std::round(784 * sx);
    const int columns = std::max(16, static_cast<int>((tx1 - tx0) / 3.0f));
    constexpr int kRows = 6;
    for (int c = 0; c < columns; ++c) {
      const float px = (static_cast<float>(c) + 0.5f) / static_cast<float>(columns);
      const float cx0 = tx0 + (tx1 - tx0) * static_cast<float>(c) / static_cast<float>(columns);
      const float cx1 = tx0 + (tx1 - tx0) * static_cast<float>(c + 1) / static_cast<float>(columns);
      for (int b = 0; b < kRows; ++b) {
        const float vt = 1.0f - static_cast<float>(b) / kRows, vb = 1.0f - static_cast<float>(b + 1) / kRows;
        const Rgba ct = track.evalColorAt(px, vt), cb = track.evalColorAt(px, vb);
        dl.addRectFilledMultiColor({cx0, y0 + (y1 - y0) * static_cast<float>(b) / kRows, cx1,
                                    y0 + (y1 - y0) * static_cast<float>(b + 1) / kRows},
                                   ct, ct, cb, cb);
      }
    }
    // The track's current position (0.5 + an animated sweep) and its waypoint markers.
    track.setPosition(0.5f + 0.5f * std::sin(t * 0.5f));
    const float bx = std::round(tx0 + track.position() * (tx1 - tx0));
    dl.addRectFilled({bx, y0, bx + 1, y1}, rgba(255, 255, 255, 150));
    for (const PaletteTrack::Waypoint& w : track.waypoints()) {
      const float hx = tx0 + w.t * (tx1 - tx0);
      dl.addRoundedRectFilled({hx - 3, y1 + 2, hx + 3, y1 + 8}, 3, rgba(165, 165, 180, 200));
    }
  }
}

}  // namespace pq::ui::detail
