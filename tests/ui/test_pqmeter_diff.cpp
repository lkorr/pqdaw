// T0.9-fix1 (M0 review M6): differential test of pq::ui's port of PQ Meter's signal-trace primitives against
// PQ Meter's own code, on fixed inputs. PQ Meter is compiled from its source tree (see pqmeter_diff.cmake;
// nothing of it is copied into this repository) against a minimal ImGui shim (pqmeter_shim/imgui.h).
//
// Compiled from PQ Meter and compared:
//   src/ui/smooth_line.h (all of it): smoothAlphaMul, catmullRom, splinePath, smoothPolyline,
//     smoothPolylineCols, smoothBand, smoothCurve
//   src/modules/spectrum.cpp helper block: withAlpha (= pq::ui::mulAlpha), setAlpha, scaleRgb, brighten,
//     gradientPolyline, gradientBand, splineSmooth, kCurveColorDefault
// Not compilable standalone, so their CALL SEQUENCES are transcribed below (refScopeTrace, refScopeEnvelope,
// refSpectrumCurve) and drive the compiled helpers above:
//   oscilloscope.cpp 860-892 (inside the drawWithTrails lambda of Oscilloscope's draw member: needs the
//     module's members, trails and ImGui state) → pq::ui::scopeTrace / scopeEnvelope
//   spectrum.cpp 1328-1395 (inside Spectrum's draw member loop) → pq::ui::spectrumCurve
//
// Results, asserted below for every input:
//   - bit-identical: the colour helpers (smoothAlphaMul, withAlpha = mulAlpha, setAlpha, scaleRgb, brighten
//     for amounts ≥ 0, the flat-fill colour), catmullRom, smoothBand, gradientBand.
//   - every geometry function: identical vertex and index counts, identical index buffers, identical vertex
//     colours and uvs. Vertex POSITIONS (and splinePath points, splineSmooth values) differ by float rounding
//     only, because the port multiplies by a reciprocal where PQ Meter divides (1/len for normals, 1/target in
//     splinePath, 1/steps and 1/knotStep for the spline parameter). Asserted: |Δ| ≤ kMaxPosDiff (1/256 px,
//     one rasteriser sub-pixel step). Measured on this machine: smoothPolyline, gradientPolyline,
//     spectrumCurve, scopeEnvelope ≤ 6.1e-5 px (1–2 ulp); smoothPolylineCols 1.2e-4 px; splinePath 3.7e-4 px;
//     smoothCurve 7.9e-4 px and scopeTrace 1.8e-3 px (the splinePath difference fed through near-degenerate
//     segments' normals); splineSmooth 5.7e-6 dB (bit-identical for power-of-two knot steps). 96–100 % of
//     coordinates are bit-identical (see the MESSAGE lines).
//   - scopeTrace / scopeEnvelope: PQ Meter's glow loop runs i = 3..1 and its i = 3 pass has alpha 0
//     (a = 0.10·glow·(1 − 1)·…), so it emits a fully transparent ribbon that the port skips. Asserted: PQ
//     Meter's output = that pass (every vertex alpha 0, so under alpha or additive blending the target is
//     unchanged) followed by the port's output.
//   - spectrumCurve flat fill: PQ Meter uses its configurable curve colour (cCurve_, default
//     kCurveColorDefault); the port uses the default. Asserted equal to the default.
//   - Not compared: draw commands / blend state (the shim has none; the port's glowBlend option defaults to
//     PQ Meter's alpha blending).
//
// PQ Meter is built without /arch (SSE2) and the port with /arch:AVX2; both are /fp:precise without FMA
// contraction, so scalar float results are the same IEEE operations in both builds.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "imgui.h"           // the shim (tests/ui/pqmeter_shim)
#include "ui/smooth_line.h"  // PQ Meter: namespace pqui

namespace pqm {
#include "pqmeter_spectrum_helpers.inc"  // PQ Meter's spectrum.cpp helpers, extracted at configure time
}  // namespace pqm

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/lines.h"

using pq::ui::DrawList;
using pq::ui::Rgba;
using pq::ui::Vec2;
using pq::ui::Vertex;

namespace {

// Largest vertex/point position difference allowed between the port and PQ Meter: 1/256 px, the sub-pixel
// grid of the rasteriser (8 bits on current GPUs; Vulkan guarantees ≥ 4). A difference below it can only move
// an edge by less than one sub-pixel step. The measured maxima are printed by report() and are far smaller.
constexpr float kMaxPosDiff = 1.0f / 256.0f;
// splineSmooth works in dB (the peak line); 1e-4 dB is < 1/4000 px on the lines scene's 2.4 px/dB axis.
constexpr float kMaxDbDiff = 1e-4f;

// --- inputs ---------------------------------------------------------------------------------------------

struct Rng {  // xorshift32
  uint32_t s = 0x9E3779B9u;
  float next() noexcept {  // [-1, 1)
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
  }
};

constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

struct Path {
  const char* name;
  std::vector<Vec2> pts;
};

// Fixed point sets covering the shapes the primitives meet: dense and coarse waveforms, a random walk, sharp
// zig-zags (bevels), a circle (every direction), exact-multiple segment lengths (splinePath's step count),
// duplicate points and reversals (degenerate normals), a dense loud trace that hits splinePath's 6000-point
// budget, and the trivial sizes.
std::vector<Path> paths() {
  std::vector<Path> out;
  {  // the lines scene's scope sine: half-pixel spacing
    Path p{"dense sine", {}};
    for (float x = 16.0f; x <= 392.0f; x += 0.5f) {
      const float ph = (x - 16.0f) / 376.0f * 2.25f * kTau;
      p.pts.push_back({x, 108.0f - 66.24f * (std::sin(ph) + 0.18f * std::sin(3.0f * ph))});
    }
    out.push_back(std::move(p));
  }
  {  // one point per 24 px
    Path p{"coarse sine", {}};
    for (int i = 0; i < 17; ++i) {
      const float u = static_cast<float>(i) / 16.0f;
      p.pts.push_back({16.0f + u * 376.0f, 108.0f + 36.4f * std::sin(u * 1.5f * kTau + 1.3f)});
    }
    out.push_back(std::move(p));
  }
  {
    Path p{"random walk", {}};
    Rng r;
    float x = 3.25f, y = 300.0f;
    for (int i = 0; i < 600; ++i) {
      p.pts.push_back({x, y});
      x += 1.0f + 2.0f * std::fabs(r.next());
      y += 6.0f * r.next();
    }
    out.push_back(std::move(p));
  }
  {  // alternating spikes, as the lines scene's zig-zag (turns sharper than 120° → bevels)
    Path p{"zig-zag", {}};
    for (int i = 0; i <= 24; ++i) {
      const float h = (i % 2 ? 1.0f : -1.0f) * (i % 4 < 2 ? 29.4f : 9.2f);
      p.pts.push_back({420.0f + static_cast<float>(i) * 14.67f, 166.9f + h});
    }
    out.push_back(std::move(p));
  }
  {
    Path p{"circle", {}};
    for (int i = 0; i <= 64; ++i) {
      const float a = static_cast<float>(i) / 64.0f * kTau;
      p.pts.push_back({200.0f + 80.0f * std::cos(a), 200.0f + 80.0f * std::sin(a)});
    }
    out.push_back(std::move(p));
  }
  {  // segment lengths at exact multiples of the spline targets (1.25, 1.5) and of 0.25
    Path p{"exact lengths", {}};
    float x = 10.0f;
    const float steps[] = {3.0f, 1.5f, 4.5f, 2.5f, 1.25f, 3.75f, 0.25f, 6.0f, 7.5f, 0.75f};
    for (int k = 0; k < 40; ++k) {
      p.pts.push_back({x, 50.0f + static_cast<float>(k % 3) * 2.5f});
      x += steps[k % 10];
    }
    out.push_back(std::move(p));
  }
  {  // duplicates, vertical runs, reversals
    Path p{"degenerate", {{10, 10}, {10, 10}, {10, 10}, {20, 10}, {20, 30}, {20, 30}, {15, 30}, {25, 31}, {25, 31},
                          {24.5f, 10}, {40, 10.5f}, {39, 10.5f}, {41, 10.5f}, {41, 10.5f}}};
    out.push_back(std::move(p));
  }
  {  // loud and dense: 4000 points with large jumps → splinePath's output budget raises the target
    Path p{"budget", {}};
    Rng r;
    for (int i = 0; i < 4000; ++i) p.pts.push_back({static_cast<float>(i) * 0.25f, 400.0f + 60.0f * r.next()});
    out.push_back(std::move(p));
  }
  out.push_back({"two points", {{1.5f, 2.5f}, {9.25f, 7.0f}}});
  out.push_back({"one point", {{1.5f, 2.5f}}});
  out.push_back({"empty", {}});
  return out;
}

std::vector<ImVec2> toIm(std::span<const Vec2> v) {
  std::vector<ImVec2> out;
  out.reserve(v.size());
  for (const Vec2& p : v) out.emplace_back(p.x, p.y);
  return out;
}

// Per-vertex colours for a path: a ramp through hue and alpha, deterministic.
std::vector<Rgba> rampColours(size_t n) {
  std::vector<Rgba> c(n);
  for (size_t i = 0; i < n; ++i)
    c[i] = pq::ui::rgba(static_cast<uint32_t>(i * 7 % 256), static_cast<uint32_t>(255 - i * 3 % 256),
                        static_cast<uint32_t>(90 + i % 160), static_cast<uint32_t>(40 + i * 13 % 216));
  return c;
}

// --- comparison -----------------------------------------------------------------------------------------

int64_t ordered(float f) {
  int32_t i;
  std::memcpy(&i, &f, 4);
  return i >= 0 ? int64_t(i) : -int64_t(i & 0x7FFFFFFF);
}
int64_t ulps(float a, float b) { return std::llabs(ordered(a) - ordered(b)); }

struct Stats {  // accumulated over a test case, for the MESSAGE
  size_t vertices = 0, coords = 0, exactCoords = 0;
  int64_t maxUlps = 0;
  float maxAbs = 0.0f;
  void add(float a, float b) {
    ++coords;
    exactCoords += a == b ? 1u : 0u;
    maxUlps = std::max(maxUlps, ulps(a, b));
    maxAbs = std::max(maxAbs, std::fabs(a - b));
  }
};

DrawList freshList() {
  DrawList dl;
  dl.reset(4096, 4096);
  ImGui::shimWhiteUv() = ImVec2(dl.whiteUv().x, dl.whiteUv().y);
  return dl;
}

// Asserts ours == ref[refVtxBegin..], ref.Idx[refIdxBegin..] (indices rebased by refVtxBegin): same counts,
// identical indices, colours and uvs; positions within kMaxPosDiff.
void checkGeometry(const DrawList& ours, const ImDrawList& ref, Stats& st, size_t refVtxBegin = 0, size_t refIdxBegin = 0) {
  const std::span<const Vertex> v = ours.vertices();
  const std::span<const uint32_t> ix = ours.indices();
  REQUIRE(ref.VtxBuffer.size() >= refVtxBegin);
  REQUIRE(ref.IdxBuffer.size() >= refIdxBegin);
  REQUIRE(v.size() == ref.VtxBuffer.size() - refVtxBegin);
  REQUIRE(ix.size() == ref.IdxBuffer.size() - refIdxBegin);
  size_t idxDiff = 0, colDiff = 0, uvDiff = 0;
  for (size_t j = 0; j < ix.size(); ++j) idxDiff += (ix[j] + refVtxBegin != ref.IdxBuffer[refIdxBegin + j]) ? 1u : 0u;
  float worst = 0.0f;
  for (size_t i = 0; i < v.size(); ++i) {
    const ImDrawVert& r = ref.VtxBuffer[refVtxBegin + i];
    colDiff += v[i].col != r.col ? 1u : 0u;
    uvDiff += (v[i].uv.x != r.uv.x || v[i].uv.y != r.uv.y) ? 1u : 0u;
    st.add(v[i].pos.x, r.pos.x);
    st.add(v[i].pos.y, r.pos.y);
    worst = std::max({worst, std::fabs(v[i].pos.x - r.pos.x), std::fabs(v[i].pos.y - r.pos.y)});
  }
  st.vertices += v.size();
  CHECK(idxDiff == 0);
  CHECK(colDiff == 0);
  CHECK(uvDiff == 0);
  CHECK(worst <= kMaxPosDiff);
}

void report(const std::string& what, const Stats& st, const char* unit = "px") {
  MESSAGE(what << ": " << st.vertices << " vertices, " << st.exactCoords << "/" << st.coords
               << " coordinates bit-identical, max " << st.maxUlps << " ulp (" << st.maxAbs << " " << std::string(unit) << ")");
}

// --- PQ Meter call sequences that live inside module members (transcribed; see the header comment) -------

// oscilloscope.cpp 860-892, non-envelope path: glow passes i = 3..1, then the core.
void refScopeTrace(ImDrawList* dl, const std::vector<ImVec2>& pts, ImU32 color, float lineWidth, float glow) {
  if (pts.size() < 2) return;
  if (glow > 0.001f) {
    const float base = std::max(1.0f, lineWidth);
    for (int i = 3; i >= 1; --i) {
      const float t = static_cast<float>(i) / 3.0f;
      const float thick = base + t * 7.0f * glow;
      const float a = 0.10f * glow * (1.0f - t) * (1.0f - t * 0.5f);
      pqui::smoothPolyline(dl, pts, pqui::smoothAlphaMul(color, a), thick, thick * 0.5f);
    }
  }
  pqui::smoothPolyline(dl, pts, color, std::max(0.6f, lineWidth));
}

// oscilloscope.cpp 860-892, envelope path: glow around both edges, the band, both edges stroked.
void refScopeEnvelope(ImDrawList* dl, const std::vector<ImVec2>& up, const std::vector<ImVec2>& dn, ImU32 color,
                      float lineWidth, float glow) {
  if (glow > 0.001f) {
    const float base = std::max(1.0f, lineWidth);
    for (int i = 3; i >= 1; --i) {
      const float t = static_cast<float>(i) / 3.0f;
      const float thick = base + t * 7.0f * glow;
      const float a = 0.10f * glow * (1.0f - t) * (1.0f - t * 0.5f);
      const ImU32 gc = pqui::smoothAlphaMul(color, a);
      pqui::smoothPolyline(dl, up, gc, thick, thick * 0.5f);
      pqui::smoothPolyline(dl, dn, gc, thick, thick * 0.5f);
    }
  }
  pqui::smoothBand(dl, up.data(), dn.data(), static_cast<int>(std::min(up.size(), dn.size())), color);
  const float ew = std::max(0.6f, lineWidth * 0.8f);
  pqui::smoothPolyline(dl, up, color, ew);
  pqui::smoothPolyline(dl, dn, color, ew);
}

// spectrum.cpp 1328-1395 for one curve (no overlay, curve style without bars, something above the floor).
void refSpectrumCurve(ImDrawList* dl, const std::vector<ImVec2>& poly, const std::vector<ImU32>& cols,
                      const std::vector<float>& lvlT, float yBottom, const pq::ui::SpectrumCurveStyle& st) {
  const int steps = static_cast<int>(poly.size());
  std::vector<ImU32> tmp(poly.size());
  if (st.fillAlpha > 0.001f) {
    if (st.gradientFill) {
      const float fa = st.fillAlpha * 0.75f * 1.0f;
      for (int s = 0; s < steps; ++s) tmp[s] = pqm::withAlpha(cols[s], fa);
    } else {
      ImVec4 fill = pqm::kCurveColorDefault;
      fill.w = st.fillAlpha;
      const ImU32 flat = ImGui::ColorConvertFloat4ToU32(fill);
      for (int s = 0; s < steps; ++s) tmp[s] = flat;
    }
    pqm::gradientBand(dl, poly.data(), tmp.data(), yBottom, st.gradientFill ? 0.0f : 1.0f, steps);
  }
  if (st.glow && st.glowAmount > 0.01f) {
    const struct {
      float mul, alpha;
    } layers[3] = {{7.0f, 0.070f}, {4.0f, 0.110f}, {2.2f, 0.170f}};
    for (const auto& L : layers) {
      for (int s = 0; s < steps; ++s) {
        const float k = lvlT[s] * lvlT[s] * lvlT[s];
        tmp[s] = pqm::withAlpha(cols[s], L.alpha * st.glowAmount * (0.15f + 0.85f * k));
      }
      pqm::gradientPolyline(dl, poly.data(), tmp.data(), steps, std::max(1.0f, st.lineWidth * L.mul));
    }
  }
  pqm::gradientPolyline(dl, poly.data(), cols.data(), steps, std::max(1.0f, st.lineWidth * 1.9f));
  for (int s = 0; s < steps; ++s) tmp[s] = pqm::brighten(cols[s], 0.30f + 0.45f * lvlT[s]);
  pqm::gradientPolyline(dl, poly.data(), tmp.data(), steps, std::max(0.8f, st.lineWidth * 0.85f));
}

// The spectrum inputs of the lines scene (pink tilt, harmonics, bump, smoothed noise, roll-off; Ice-like
// colours with the floor fade in alpha), on a 768 × 184 panel.
struct SpectrumInput {
  std::vector<Vec2> poly;
  std::vector<Rgba> cols;
  std::vector<float> lvl, peakDb;
  float yBottom = 400.0f;
};
SpectrumInput spectrumInput(uint32_t steps) {
  SpectrumInput in;
  const float x0 = 16.0f, w = 768.0f, y0 = 216.0f, h = 184.0f, floorDb = -78.0f;
  Rng rng;
  float noise = 0.0f;
  for (uint32_t s = 0; s < steps; ++s) {
    const float u = static_cast<float>(s) / static_cast<float>(steps - 1);
    const float f = 20.0f * std::pow(1000.0f, u);
    float db = -44.0f - 3.0f * std::log2(f / 1000.0f) - 6.0f * u;
    for (int k = 1; k <= 12; ++k) {
      const float d = std::log2(f / (110.0f * static_cast<float>(k))) * 40.0f;
      db += (30.0f - 1.8f * static_cast<float>(k)) * std::exp(-d * d);
    }
    noise = 0.6f * noise + 0.4f * rng.next();
    db += 4.0f * noise;
    if (f > 12000.0f) db -= (f - 12000.0f) * 0.006f;
    const float lt = std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
    const float vis = std::clamp((db - floorDb) / 3.12f, 0.0f, 1.0f);
    in.poly.push_back({x0 + u * w, y0 + (1.0f - lt) * h});
    in.lvl.push_back(lt);
    in.cols.push_back(pq::ui::mulAlpha(pq::ui::rgba(static_cast<uint32_t>(40 + 200 * lt), static_cast<uint32_t>(120 + 120 * lt), 255),
                                       vis));
    in.peakDb.push_back(db + 4.0f);
  }
  return in;
}

}  // namespace

TEST_CASE("pqmeter diff: colour helpers are bit-identical") {
  const float factors[] = {-0.5f, 0.0f, 0.001f, 0.07f, 0.0875f, 0.11f, 0.17f, 0.25f, 1.0f / 3.0f, 0.375f, 0.5f,
                           0.6f, 0.75f, 0.999f, 1.0f, 1.35f, 2.0f, 255.0f};
  size_t checked = 0, smoothMul = 0, mul = 0, set = 0, scale = 0, bright = 0;
  for (uint32_t a = 0; a < 256; ++a)
    for (uint32_t v = 0; v < 256; v += 3) {
      const Rgba c = pq::ui::rgba(v, (v * 37 + 11) & 255u, (v * 101 + 7) & 255u, a);
      for (float k : factors) {
        smoothMul += pq::ui::smoothAlphaMul(c, k) != pqui::smoothAlphaMul(c, k) ? 1u : 0u;
        mul += pq::ui::mulAlpha(c, k) != pqm::withAlpha(c, k) ? 1u : 0u;
        set += pq::ui::setAlpha(c, k) != pqm::setAlpha(c, k) ? 1u : 0u;
        scale += pq::ui::scaleRgb(c, k) != pqm::scaleRgb(c, k) ? 1u : 0u;
        // brighten's amount is ≥ 0 in use (0.30 + 0.45·level); a negative one makes both versions convert a
        // negative float to an unsigned channel (PQ Meter via int and IM_COL32, the port via uint32_t), which
        // is outside the function's domain.
        if (k >= 0.0f) bright += pq::ui::brighten(c, k) != pqm::brighten(c, k) ? 1u : 0u;
        ++checked;
      }
    }
  MESSAGE(checked << " colour/factor pairs per helper");
  CHECK(smoothMul == 0);
  CHECK(mul == 0);
  CHECK(set == 0);
  CHECK(scale == 0);
  CHECK(bright == 0);
  // spectrumCurve's flat fill: PQ Meter's default curve colour at fillAlpha, as ImGui converts it.
  for (float fa : {0.0f, 0.1f, 0.35f, 0.5f, 0.77f, 1.0f}) {
    ImVec4 fill = pqm::kCurveColorDefault;
    fill.w = fa;
    CHECK(pq::ui::setAlpha(pq::ui::rgba(110, 217, 255), fa) == ImGui::ColorConvertFloat4ToU32(fill));
  }
}

TEST_CASE("pqmeter diff: catmullRom is bit-identical") {
  Rng r;
  size_t diff = 0;
  for (int i = 0; i < 20000; ++i) {
    const Vec2 p[4] = {{500 * r.next(), 500 * r.next()}, {500 * r.next(), 500 * r.next()}, {500 * r.next(), 500 * r.next()},
                       {500 * r.next(), 500 * r.next()}};
    const float t = 0.5f + 0.5f * r.next();
    const Vec2 a = pq::ui::catmullRom(p[0], p[1], p[2], p[3], t);
    const ImVec2 b = pqui::catmullRom({p[0].x, p[0].y}, {p[1].x, p[1].y}, {p[2].x, p[2].y}, {p[3].x, p[3].y}, t);
    diff += (a.x != b.x || a.y != b.y) ? 1u : 0u;
  }
  CHECK(diff == 0);
}

TEST_CASE("pqmeter diff: splinePath (same point count; points within kMaxPosDiff)") {
  Stats st;
  for (const Path& p : paths())
    for (float seg : {1.25f, 1.5f, 0.1f, 3.0f}) {
      CAPTURE(p.name);
      CAPTURE(seg);
      std::vector<Vec2> ours;
      pq::ui::splinePath(p.pts, ours, seg);
      std::vector<ImVec2> ref;
      pqui::splinePath(toIm(p.pts), ref, seg);
      REQUIRE(ours.size() == ref.size());
      float worst = 0.0f;
      for (size_t i = 0; i < ours.size(); ++i) {
        st.add(ours[i].x, ref[i].x);
        st.add(ours[i].y, ref[i].y);
        worst = std::max({worst, std::fabs(ours[i].x - ref[i].x), std::fabs(ours[i].y - ref[i].y)});
      }
      st.vertices += ours.size();
      CHECK(worst <= kMaxPosDiff);
    }
  report("splinePath", st);
}

TEST_CASE("pqmeter diff: smoothPolyline and smoothCurve") {
  Stats st, stCurve;
  const Rgba col = pq::ui::rgba(120, 220, 160, 230);
  for (const Path& p : paths())
    for (float thick : {0.0f, 0.05f, 0.6f, 1.0f, 1.4f, 2.5f, 3.4f})
      for (float feather : {0.75f, 0.01f, 1.7f}) {
        CAPTURE(p.name);
        CAPTURE(thick);
        CAPTURE(feather);
        DrawList dl = freshList();
        pq::ui::smoothPolyline(dl, p.pts, col, thick, feather);
        ImDrawList ref;
        pqui::smoothPolyline(&ref, toIm(p.pts), col, thick, feather);
        checkGeometry(dl, ref, st);

        DrawList dl2 = freshList();
        pq::ui::smoothCurve(dl2, p.pts, col, thick, 1.25f, feather);
        ImDrawList ref2;
        pqui::smoothCurve(&ref2, toIm(p.pts), col, thick, 1.25f, feather);
        checkGeometry(dl2, ref2, stCurve);
      }
  report("smoothPolyline", st);
  report("smoothCurve", stCurve);
}

TEST_CASE("pqmeter diff: smoothPolylineCols (bevels and miters)") {
  Stats st;
  size_t bevelled = 0;
  for (const Path& p : paths())
    for (float thick : {0.5f, 1.6f, 4.0f})
      for (float feather : {0.75f, 0.2f}) {
        CAPTURE(p.name);
        CAPTURE(thick);
        CAPTURE(feather);
        const std::vector<Rgba> cols = rampColours(p.pts.size());
        const uint32_t n = static_cast<uint32_t>(p.pts.size());
        DrawList dl = freshList();
        pq::ui::smoothPolylineCols(dl, p.pts.data(), cols.data(), n, thick, feather);
        ImDrawList ref;
        const std::vector<ImVec2> im = toIm(p.pts);
        pqui::smoothPolylineCols(&ref, im.data(), cols.data(), static_cast<int>(n), thick, feather);
        checkGeometry(dl, ref, st);
        if (n >= 2) bevelled += (dl.vertices().size() - (n - 1) * 8) / 5;
      }
  MESSAGE("bevels emitted: " << bevelled);
  CHECK(bevelled > 0);  // the zig-zag and degenerate paths exercise the bevel path
  report("smoothPolylineCols", st);
}

TEST_CASE("pqmeter diff: smoothBand is bit-identical") {
  Stats st;
  Rng r;
  for (uint32_t n : {0u, 1u, 2u, 3u, 376u}) {
    std::vector<Vec2> up(n), dn(n);
    for (uint32_t c = 0; c < n; ++c) {
      const float x = 408.0f + static_cast<float>(c) + 0.5f;
      const float a = 50.0f + 30.0f * r.next(), b = 50.0f + 30.0f * r.next();  // edges cross where a > b
      up[c] = {x, a};
      dn[c] = {x, b};
    }
    for (float feather : {0.75f, 0.0f, 2.0f}) {
      CAPTURE(n);
      CAPTURE(feather);
      DrawList dl = freshList();
      pq::ui::smoothBand(dl, up.data(), dn.data(), n, pq::ui::rgba(120, 190, 255, 200), feather);
      ImDrawList ref;
      const std::vector<ImVec2> iu = toIm(up), id = toIm(dn);
      pqui::smoothBand(&ref, iu.data(), id.data(), static_cast<int>(n), pq::ui::rgba(120, 190, 255, 200), feather);
      checkGeometry(dl, ref, st);
    }
  }
  report("smoothBand", st);
  CHECK(st.exactCoords == st.coords);
}

TEST_CASE("pqmeter diff: gradientPolyline, gradientBand, splineSmooth") {
  Stats stLine, stBand, stSmooth;
  for (uint32_t steps : {2u, 3u, 33u, 768u, 1024u}) {
    CAPTURE(steps);
    const SpectrumInput in = spectrumInput(steps);
    const std::vector<ImVec2> poly = toIm(in.poly);
    for (float thick : {0.0f, 0.8f, 1.36f, 3.04f, 11.2f}) {
      DrawList dl = freshList();
      pq::ui::gradientPolyline(dl, in.poly.data(), in.cols.data(), steps, thick);
      ImDrawList ref;
      pqm::gradientPolyline(&ref, poly.data(), in.cols.data(), static_cast<int>(steps), thick);
      checkGeometry(dl, ref, stLine);
    }
    for (float bottom : {0.0f, 0.4f, 1.0f, -1.0f, 2.0f}) {
      DrawList dl = freshList();
      pq::ui::gradientBand(dl, in.poly.data(), in.cols.data(), in.yBottom, bottom, steps);
      ImDrawList ref;
      pqm::gradientBand(&ref, poly.data(), in.cols.data(), in.yBottom, bottom, static_cast<int>(steps));
      checkGeometry(dl, ref, stBand);
    }
    for (int knot : {0, 1, 2, 3, 5, 8, 12, 16, 600}) {
      CAPTURE(knot);
      std::vector<float> ours(steps), ref(steps);
      pq::ui::splineSmooth(in.peakDb.data(), ours.data(), steps, knot);
      pqm::splineSmooth(in.peakDb.data(), ref.data(), static_cast<int>(steps), knot);
      float worst = 0.0f;
      for (uint32_t s = 0; s < steps; ++s) {
        stSmooth.add(ours[s], ref[s]);
        worst = std::max(worst, std::fabs(ours[s] - ref[s]));
      }
      stSmooth.vertices += steps;
      CHECK(worst <= kMaxDbDiff);
    }
  }
  report("gradientPolyline", stLine);
  report("gradientBand", stBand);
  report("splineSmooth (values)", stSmooth, "dB");
  CHECK(stBand.exactCoords == stBand.coords);
}

TEST_CASE("pqmeter diff: scopeTrace = PQ Meter's trace minus its alpha-0 glow pass") {
  Stats st;
  for (const Path& p : paths()) {
    std::vector<Vec2> smoothed;
    pq::ui::splinePath(p.pts, smoothed, 1.25f);
    std::vector<ImVec2> refSmoothed;
    pqui::splinePath(toIm(p.pts), refSmoothed, 1.25f);
    for (float lineWidth : {0.5f, 1.0f, 1.4f, 3.0f})
      for (float glow : {0.0f, 0.35f, 0.6f, 1.0f}) {
        CAPTURE(p.name);
        CAPTURE(lineWidth);
        CAPTURE(glow);
        const Rgba color = pq::ui::rgba(120, 220, 160, 255);
        DrawList dl = freshList();
        pq::ui::scopeTrace(dl, smoothed, color, lineWidth, glow);
        ImDrawList ref;
        refScopeTrace(&ref, refSmoothed, color, lineWidth, glow);
        // The skipped pass: one ribbon (8 vertices / 18 indices per segment), every vertex alpha 0.
        size_t skipV = 0, skipI = 0;
        if (glow > 0.001f && refSmoothed.size() >= 2) {
          skipV = (refSmoothed.size() - 1) * 8;
          skipI = (refSmoothed.size() - 1) * 18;
          REQUIRE(ref.VtxBuffer.size() >= skipV);
          size_t visible = 0;
          for (size_t i = 0; i < skipV; ++i) visible += (ref.VtxBuffer[i].col >> IM_COL32_A_SHIFT) != 0 ? 1u : 0u;
          CHECK(visible == 0);
        }
        checkGeometry(dl, ref, st, skipV, skipI);
      }
  }
  report("scopeTrace", st);
}

TEST_CASE("pqmeter diff: scopeEnvelope = PQ Meter's envelope minus its alpha-0 glow pass") {
  Stats st;
  Rng rng;
  for (uint32_t n : {2u, 5u, 376u}) {
    std::vector<Vec2> up(n), dn(n);
    for (uint32_t c = 0; c < n; ++c) {
      const float x = 408.5f + static_cast<float>(c);
      const float mid = 70.0f + 20.0f * std::sin(static_cast<float>(c) * 0.13f), spread = 1.0f + 15.0f * std::fabs(rng.next());
      up[c] = {x, mid - spread};
      dn[c] = {x, mid + spread};
    }
    for (float lineWidth : {0.5f, 1.4f, 2.0f})
      for (float glow : {0.0f, 0.35f, 1.0f}) {
        CAPTURE(n);
        CAPTURE(lineWidth);
        CAPTURE(glow);
        const Rgba color = pq::ui::rgba(120, 190, 255, 200);
        DrawList dl = freshList();
        pq::ui::scopeEnvelope(dl, up.data(), dn.data(), n, color, lineWidth, glow);
        ImDrawList ref;
        refScopeEnvelope(&ref, toIm(up), toIm(dn), color, lineWidth, glow);
        size_t skipV = 0, skipI = 0;
        if (glow > 0.001f) {  // i = 3: two ribbons (up, dn), alpha 0
          skipV = 2 * (n - 1) * 8;
          skipI = 2 * (n - 1) * 18;
          size_t visible = 0;
          for (size_t i = 0; i < skipV; ++i) visible += (ref.VtxBuffer[i].col >> IM_COL32_A_SHIFT) != 0 ? 1u : 0u;
          CHECK(visible == 0);
        }
        checkGeometry(dl, ref, st, skipV, skipI);
      }
  }
  report("scopeEnvelope", st);
}

TEST_CASE("pqmeter diff: spectrumCurve = PQ Meter's single-curve layering") {
  Stats st;
  for (uint32_t steps : {2u, 64u, 768u}) {
    const SpectrumInput in = spectrumInput(steps);
    const std::vector<ImVec2> poly = toIm(in.poly);
    const std::vector<ImU32> cols(in.cols.begin(), in.cols.end());
    for (bool gradient : {true, false})
      for (bool glow : {true, false})
        for (float glowAmount : {0.005f, 0.5f, 1.0f, 2.0f})
          for (float fillAlpha : {0.0f, 0.5f, 0.9f})
            for (float lineWidth : {0.4f, 1.6f, 3.0f}) {
              CAPTURE(steps);
              CAPTURE(gradient);
              CAPTURE(glow);
              CAPTURE(glowAmount);
              CAPTURE(fillAlpha);
              CAPTURE(lineWidth);
              pq::ui::SpectrumCurveStyle style;
              style.gradientFill = gradient;
              style.glow = glow;
              style.glowAmount = glowAmount;
              style.fillAlpha = fillAlpha;
              style.lineWidth = lineWidth;
              DrawList dl = freshList();
              pq::ui::spectrumCurve(dl, in.poly.data(), in.cols.data(), in.lvl.data(), steps, in.yBottom, style);
              ImDrawList ref;
              refSpectrumCurve(&ref, poly, cols, in.lvl, in.yBottom, style);
              checkGeometry(dl, ref, st);
            }
  }
  report("spectrumCurve", st);
}
