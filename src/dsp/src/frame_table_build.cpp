// The frame-table builder (TMM.1): keyframes → dense frames → bake → mip levels. See frame_table_build.h.

#include "pq/dsp/frame_table_build.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

#include "fft_mixed.h"
#include "frame_table_ops.h"
#include "pq/dsp/wave_live.h"

namespace pq::dsp {

namespace {

using detail::Cd;
using detail::MixedFft;
constexpr uint32_t kH = kFrameTableMaxHarmonic;  // 1023
constexpr uint32_t kBins = kH + 1;               // c_0 … c_1023
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr uint32_t kMaxBreakpoints = 2 * 17 + 2;

// A value with its first two derivatives (forward-mode, 2nd order), for the bake's jump analysis. The warp / fold /
// sync templates (wave_warp.h) are instantiated on it; floor and sin are found by argument-dependent lookup.
struct Jet {
  double v = 0, d1 = 0, d2 = 0;
  Jet() = default;
  Jet(double x) : v(x) {}  // NOLINT: constants in the formulas are T(c)
  Jet(double x, double a, double b) : v(x), d1(a), d2(b) {}
};
Jet operator+(Jet a, Jet b) { return {a.v + b.v, a.d1 + b.d1, a.d2 + b.d2}; }
Jet operator-(Jet a, Jet b) { return {a.v - b.v, a.d1 - b.d1, a.d2 - b.d2}; }
Jet operator*(Jet a, Jet b) { return {a.v * b.v, a.d1 * b.v + a.v * b.d1, a.d2 * b.v + 2.0 * a.d1 * b.d1 + a.v * b.d2}; }
Jet operator/(Jet a, Jet b) {
  const double q = a.v / b.v;
  const double q1 = (a.d1 - q * b.d1) / b.v;
  return {q, q1, (a.d2 - 2.0 * q1 * b.d1 - q * b.d2) / b.v};
}
bool operator<(Jet a, Jet b) { return a.v < b.v; }
Jet floor(Jet a) { return {std::floor(a.v), 0.0, 0.0}; }
Jet sin(Jet a) {
  const double s = std::sin(a.v), c = std::cos(a.v);
  return {s, c * a.d1, -s * a.d1 * a.d1 + c * a.d2};
}

// The 6 weights of the interpolator at fraction x for y_{−2} … y_3 (frame_table_ops.h).
void interpWeights(double x, double* w) noexcept {
  const double z = x - 0.5;
  double e[3], o[3];
  for (int d = 0; d < 3; ++d) {
    const auto& c = detail::ft::kInterp[d];
    e[d] = c[0] + z * z * (c[2] + z * z * c[4]);
    o[d] = z * (c[1] + z * z * (c[3] + z * z * c[5]));
  }
  w[0] = e[2] + o[2];
  w[1] = e[1] + o[1];
  w[2] = e[0] + o[0];
  w[3] = e[0] - o[0];
  w[4] = e[1] - o[1];
  w[5] = e[2] - o[2];
}

Error invalid(std::string msg) { return Error{ErrorCode::InvalidArgument, std::move(msg), {}}; }

}  // namespace

struct FrameTableBuilder::Impl {
  // ---- sized arenas (allocated only when a spec needs more than the current size) ----
  double rate = 0;
  FrameTableLayout layout;
  MixedFft fftCycle;                     // 2048
  std::vector<MixedFft> fftLevel;        // per distinct level length
  std::vector<uint32_t> fftOfLevel;      // level → fftLevel index
  std::vector<std::vector<double>> preEmph;  // level → 1 / H(h / len), h ≤ H_j
  uint32_t capKeys = 0, capFrames = 0;
  std::vector<Cd> keySpec;               // capKeys × kBins
  std::vector<double> keyMag, keyArg;
  std::vector<Cd> dense;                 // capFrames × kBins
  uint32_t lastFrames = 0;
  // TMM.16: the dense frames this build computes, ascending (all of them for buildInto; a subset for buildFramesInto).
  std::vector<uint32_t> sel;             // capacity capFrames
  uint32_t selCount = 0;
  std::vector<Cd> bufIn, bufOut;         // max transform length
  std::vector<detail::Cd4> buf4In, buf4Out;  // the longest level

  // bake
  uint32_t bakeM = 0;
  MixedFft fftBake[6];                   // resolutions 4 … 128
  MixedFft* bakeFft = nullptr;
  std::vector<double> bakeEmph;          // 1 / H(h / M), h ≤ 1023
  std::vector<double> xa, xb;            // M + 8 (4 guards each side)
  std::vector<int32_t> tapIdx[2];        // M: index of y_{−2} in the guarded array
  std::vector<double> tapW[2];           // M × 6
  bool tapsValid = false;
  FrameBake tapsFor{};
  uint32_t tapsM = 0;
  double mixB = 0;
  uint32_t numBp = 0;
  double bp[kMaxBreakpoints]{};
  Jet bpOmega[kMaxBreakpoints][2][2]{};  // [breakpoint][left, right][tap a, b]: read phase jets at φ_b ∓ η
  double corr[kMaxBreakpoints][3]{};     // per frame: jumps of value, slope, curvature
  double corrPair[2][kMaxBreakpoints][3]{};  // the two frames of a bakePair
  double piece[kMaxBreakpoints][4]{};    // per frame: the correction cubic on each piece

  Status ensure(const FrameTableSpec& spec);
  void keyframeSpectra(const FrameTableSpec& spec);
  void denseFrames(const FrameTableSpec& spec);
  void prepareTaps(const FrameBake& b);
  void bakePair(const FrameBake& b, Cd* fa, Cd* fb);
  void jumps(const FrameBake& b, const Cd* c);
  void levels(FrameTable& out, bool antiderivative);
  void edges(FrameTable& out);
  void sines(FrameTable& out);
  Status run(const FrameTableSpec& spec, FrameTable& out, bool all);  // TMM.16: the steps over `sel`
  // TMM.8-fix: the live-band bake (FrameTableSpec::liveBand).
  Status ensureLive(const FrameTableSpec& spec);
  void liveBandLevels(const FrameTableSpec& spec, FrameTable& out);
  FrameTable liveSrc;                    // the unbaked frames' own levels (the source the live read reads)
  std::vector<MixedFft> fftLive;         // per distinct level: 2·len_j
  std::vector<uint32_t> fftLiveOf;       // level → fftLive index
  std::vector<float> livePhase, liveY;   // 2·len_0
  std::vector<Cd> liveIn, liveOut;       // 2·len_0
  std::vector<Cd> liveSpec;              // kBins: one level's harmonics
  std::vector<float> levelTop;           // per level: its representative increment e_j
};

Status FrameTableBuilder::Impl::ensure(const FrameTableSpec& spec) {
  if (rate != spec.sampleRate) {
    auto l = frameTableLayout(spec.sampleRate);
    if (!l) return std::move(l).error();
    layout = std::move(l.value());
    rate = spec.sampleRate;
    fftLevel.clear();
    fftOfLevel.assign(layout.levels, 0);
    preEmph.assign(layout.levels, {});
    uint32_t maxLen = kKeyframeCycle;
    for (uint32_t j = 0; j < layout.levels; ++j) {
      const uint32_t len = layout.length[j];
      maxLen = std::max(maxLen, len);
      if (j == 0 || len != layout.length[j - 1]) {
        fftLevel.emplace_back();
        if (!fftLevel.back().init(len)) return Error{ErrorCode::Unknown, "frame table level length not 7-smooth", {}};
      }
      fftOfLevel[j] = static_cast<uint32_t>(fftLevel.size() - 1);
      auto& e = preEmph[j];
      e.resize(layout.top[j] + 1);
      for (uint32_t h = 0; h <= layout.top[j]; ++h) e[h] = 1.0 / frameInterpResponse(static_cast<double>(h) / len);
    }
    if (fftCycle.size() != kKeyframeCycle) fftCycle.init(kKeyframeCycle);
    if (buf4In.size() < maxLen) {
      buf4In.resize(maxLen);
      buf4Out.resize(maxLen);
    }
    if (bufIn.size() < maxLen) {
      bufIn.resize(maxLen);
      bufOut.resize(maxLen);
    }
  }
  if (spec.liveBand && spec.bake.active() && !spec.antiderivative)
    if (auto s = ensureLive(spec); !s) return s;
  const uint32_t keys = static_cast<uint32_t>(spec.keyframes.size());
  if (keys > capKeys) {
    capKeys = keys;
    keySpec.resize(static_cast<std::size_t>(capKeys) * kBins);
    keyMag.resize(keySpec.size());
    keyArg.resize(keySpec.size());
  }
  if (spec.denseFrames > capFrames) {
    capFrames = spec.denseFrames;
    dense.resize(static_cast<std::size_t>(capFrames) * kBins);
    sel.resize(capFrames);
  }
  if (spec.bake.active()) {
    const uint32_t r = frameBakeResolution(spec);
    const uint32_t m = kKeyframeCycle * r;
    MixedFft& fft = fftBake[std::countr_zero(r) - 2];  // one plan per resolution, kept
    if (fft.size() != m && !fft.init(m)) return Error{ErrorCode::Unknown, "bake length not 7-smooth", {}};
    if (xa.size() < m + 2 * kFrameTableGuard) {
      xa.resize(m + 2 * kFrameTableGuard);
      xb.resize(m + 2 * kFrameTableGuard);
      for (auto& v : tapIdx) v.resize(m);
      for (auto& v : tapW) v.resize(static_cast<std::size_t>(m) * 6);
    }
    if (m != bakeM) {
      bakeM = m;
      bakeFft = &fft;
      bakeEmph.resize(kBins);
      for (uint32_t h = 0; h < kBins; ++h) bakeEmph[h] = 1.0 / frameInterpResponse(static_cast<double>(h) / m);
      tapsValid = false;
    }
    if (bufIn.size() < m) {
      bufIn.resize(m);
      bufOut.resize(m);
    }
  }
  return Ok{};
}

void FrameTableBuilder::Impl::keyframeSpectra(const FrameTableSpec& spec) {
  const uint32_t n = kKeyframeCycle;
  const uint32_t keys = static_cast<uint32_t>(spec.keyframes.size());
  auto gain = [&](uint32_t k) {
    if (!spec.normalise) return 1.0;
    float peak = 0;
    for (const float v : spec.keyframes[k].cycle) peak = std::max(peak, std::fabs(v));
    return peak > 0 ? 1.0 / peak : 1.0;
  };
  for (uint32_t k = 0; k < keys; k += 2) {
    const bool two = k + 1 < keys;
    const double ga = gain(k), gb = two ? gain(k + 1) : 0.0;
    const float* a = spec.keyframes[k].cycle.data();
    const float* b = two ? spec.keyframes[k + 1].cycle.data() : nullptr;
    for (uint32_t i = 0; i < n; ++i) bufIn[i] = Cd(a[i] * ga, b ? b[i] * gb : 0.0);
    fftCycle.forward(bufIn.data(), bufOut.data());
    for (uint32_t h = 0; h < kBins; ++h) {
      const Cd z = bufOut[h], zc = std::conj(bufOut[(n - h) % n]);
      const Cd ca = (z + zc) * (0.5 / n);
      const Cd cb = (z - zc) * Cd(0.0, -0.5 / n);  // (z − zc) / (2i)
      const std::size_t ia = static_cast<std::size_t>(k) * kBins + h;
      keySpec[ia] = h == 0 ? Cd(ca.real(), 0.0) : ca;
      if (two) keySpec[ia + kBins] = h == 0 ? Cd(cb.real(), 0.0) : cb;
    }
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(keys) * kBins; ++i) {
    keyMag[i] = std::abs(keySpec[i]);
    keyArg[i] = std::arg(keySpec[i]);
  }
}

void FrameTableBuilder::Impl::denseFrames(const FrameTableSpec& spec) {
  const uint32_t keys = static_cast<uint32_t>(spec.keyframes.size());
  const uint32_t frames = spec.denseFrames;
  for (uint32_t si = 0; si < selCount; ++si) {
    const uint32_t d = sel[si];
    const double u = frames > 1 ? static_cast<double>(d) / (frames - 1) : 0.0;
    Cd* out = dense.data() + static_cast<std::size_t>(d) * kBins;
    // The gap: the last keyframe at or before u whose successor is after u.
    uint32_t a = 0;
    while (a + 1 < keys && static_cast<double>(spec.keyframes[a + 1].position) <= u) ++a;
    const double pa = spec.keyframes[a].position;
    double t = 0.0;
    if (a + 1 < keys && u > pa) t = (u - pa) / (static_cast<double>(spec.keyframes[a + 1].position) - pa);
    constexpr double kSnap = 1e-7;  // a dense frame on a keyframe (within float position rounding) is that keyframe
    uint32_t copy = a;
    if (t > 1.0 - kSnap) copy = a + 1, t = 0.0;
    const Cd* ka = keySpec.data() + static_cast<std::size_t>(copy) * kBins;
    if (t <= kSnap) {
      std::copy(ka, ka + kBins, out);
      continue;
    }
    const Cd* kb = ka + kBins;
    const GapMode mode = spec.gaps.empty() ? GapMode::Spectral : spec.gaps[a];
    if (mode == GapMode::Crossfade) {
      for (uint32_t h = 0; h < kBins; ++h) out[h] = ka[h] + t * (kb[h] - ka[h]);
      continue;
    }
    const double* ma = keyMag.data() + static_cast<std::size_t>(a) * kBins;
    const double* aa = keyArg.data() + static_cast<std::size_t>(a) * kBins;
    out[0] = Cd(ka[0].real() + t * (kb[0].real() - ka[0].real()), 0.0);
    for (uint32_t h = 1; h < kBins; ++h) {
      const double m0 = ma[h], m1 = ma[h + kBins];
      double p0 = aa[h], p1 = aa[h + kBins];
      if (m0 == 0.0) p0 = p1;
      if (m1 == 0.0) p1 = p0;
      double dp = std::remainder(p1 - p0, kTwoPi);  // [−π, π]
      // Exactly opposite (within kOppositePhaseTol: the keyframes' float samples put rounding noise on every phase,
      // which must not pick the direction harmonic by harmonic) turns counter-clockwise (TMM.16: a saw → square
      // morph turned each odd harmonic whichever way its rounding said, an incoherent wave).
      if (std::fabs(dp) >= std::numbers::pi - kOppositePhaseTol) dp = std::numbers::pi;
      out[h] = std::polar(m0 + t * (m1 - m0), p0 + t * dp);
    }
  }
  if (spec.bake.active() && !(spec.liveBand && !spec.antiderivative)) {
    for (uint32_t si = 0; si < selCount; si += 2) {  // a full build pairs (0, 1), (2, 3), ... as before
      Cd* fa = dense.data() + static_cast<std::size_t>(sel[si]) * kBins;
      bakePair(spec.bake, fa, si + 1 < selCount ? dense.data() + static_cast<std::size_t>(sel[si + 1]) * kBins : nullptr);
    }
  }
  if (spec.removeDc && !(spec.bake.active() && spec.liveBand && !spec.antiderivative))
    for (uint32_t si = 0; si < selCount; ++si) dense[static_cast<std::size_t>(sel[si]) * kBins] = 0.0;
}

Status FrameTableBuilder::Impl::ensureLive(const FrameTableSpec& spec) {
  // The FFT plans and buffers (sized once per rate) and the source table (per frame count).
  if (fftLiveOf.size() != layout.levels || fftLive.empty() || liveSrc.layout().sampleRate != layout.sampleRate) {
    fftLive.clear();
    fftLiveOf.assign(layout.levels, 0);
    uint32_t maxM = 0;
    for (uint32_t j = 0; j < layout.levels; ++j) {
      const uint32_t m = 2 * layout.length[j];
      maxM = std::max(maxM, m);
      if (j == 0 || layout.length[j] != layout.length[j - 1]) {
        fftLive.emplace_back();
        if (!fftLive.back().init(m)) return Error{ErrorCode::Unknown, "live-band bake length not 7-smooth", {}};
      }
      fftLiveOf[j] = static_cast<uint32_t>(fftLive.size() - 1);
    }
    livePhase.resize(maxM);
    liveY.resize(maxM);
    liveIn.resize(maxM);
    liveOut.resize(maxM);
    liveSpec.resize(kBins);
    liveSrc = FrameTable{};
  }
  if (liveSrc.frames() != spec.denseFrames || liveSrc.layout().sampleRate != layout.sampleRate) {
    auto t = FrameTable::allocate(layout, spec.denseFrames);
    if (!t) return std::move(t).error();
    liveSrc = std::move(t.value());
    // e_j: the top of the last increment bucket holding level j (as the kernels' bucketTop).
    levelTop.assign(layout.levels, 0.0f);
    const FrameTableView& v = liveSrc.view();
    for (uint32_t b = 0; b < kFrameLevelBuckets; ++b) levelTop[v.bucketLevel[b]] = v.bucketTop[b];
  }
  return Ok{};
}

// Each level of each frame: the live read at the level's representative increment over the source levels.
void FrameTableBuilder::Impl::liveBandLevels(const FrameTableSpec& spec, FrameTable& out) {
  const FrameBake& b = spec.bake;
  const LiveControls c{b.warpMode, b.warp, b.fold, b.sync};
  const double s = 1.0 + (kSyncMaxRatio - 1.0) * static_cast<double>(b.sync);
  const double scale = liveSpeedScale(b.fold);
  double liveBp[kMaxBreakpoints];
  const uint32_t numLiveBp = warpBreakpoints(b.warpMode, b.warp, b.sync, liveBp, kMaxBreakpoints);
  const FrameTableView& src = liveSrc.view();
  const uint32_t g = kFrameTableGuard;
  for (uint32_t si = 0; si < selCount; ++si) {
    const uint32_t d = sel[si];
    for (uint32_t j = 0; j < layout.levels; ++j) {
      if (j > 0 && layout.offset[j] == layout.offset[j - 1]) continue;
      const uint32_t len = layout.length[j], top = layout.top[j], m = 2 * len;
      const float e = levelTop[j];
      for (uint32_t i = 0; i < m; ++i) livePhase[i] = static_cast<float>((i + 0.5) / m);
      liveRenderStatic(src, d, c, e, livePhase.data(), m, liveY.data());
      // The breakpoints' value and slope jumps (per unit φ), each side read at its own level speeds.
      double jv[kMaxBreakpoints], kv[kMaxBreakpoints];
      for (uint32_t q = 0; q < numLiveBp; ++q) {
        const double phiB = liveBp[q] * s;
        const bool start = std::fabs(phiB - std::round(phiB)) < 1e-9;  // a sub-period start (ψ wraps)
        double left = start ? 1.0 : phiB - std::floor(phiB), right = start ? 0.0 : left;
        if (start && liveBp[q] == 0.0) {  // φ = 0: the cycle's end reads ψ = frac(s) (a sync reset)
          left = s - std::floor(s);
          if (left == 0.0) left = 1.0;
        }
        double gl = 0, gr = 0;
        // liveValueAtPsi reads frame 0 of a view: point it at frame d.
        FrameTableView fv = src;
        fv.data = src.data + static_cast<std::size_t>(d) * src.frameStride;
        fv.frames = 1;
        auto sideF = [&](double psi, int sd, double& slope) {
          const LiveTaps tp = liveTaps(b.warpMode, psi, b.warp, sd);
          return liveValueAtPsi(fv, 0.0f, c, psi, e, sd, &slope, e * s * std::fabs(tp.da) * scale, e * s * scale);
        };
        const double yl = sideF(left, -1, gl), yr = sideF(right, +1, gr);
        jv[q] = yr - yl;
        kv[q] = (gr - gl) * s;
      }
      // The remainder (C1 at every breakpoint), transformed.
      for (uint32_t i = 0; i < m; ++i) {
        const double phi = (i + 0.5) / m;
        double r = liveY[i];
        for (uint32_t q = 0; q < numLiveBp; ++q) {
          double x = phi - liveBp[q];
          x -= std::floor(x);
          r -= jv[q] * (0.5 - x) - kv[q] * 0.5 * (x * x - x + 1.0 / 6.0);
        }
        liveIn[i] = Cd(r, 0.0);
      }
      fftLive[fftLiveOf[j]].forward(liveIn.data(), liveOut.data());
      for (uint32_t h = 0; h <= top; ++h) {
        Cd ch = liveOut[h] * std::polar(1.0 / m, -std::numbers::pi * h / m);
        if (h > 0) {
          const double w = 1.0 / (kTwoPi * h);  // 1/(2πih) = −i·w, 1/(2πih)² = −w²
          for (uint32_t q = 0; q < numLiveBp; ++q) {
            const Cd ph = std::polar(1.0, -kTwoPi * h * liveBp[q]);
            ch += ph * Cd(-kv[q] * w * w, -jv[q] * w);
          }
        } else {
          ch = Cd(spec.removeDc ? 0.0 : ch.real(), 0.0);
        }
        liveSpec[h] = ch;
      }
      if (j == 0) {  // frameSpectrum(): the frame after the bake (at the full band)
        Cd* dd = dense.data() + static_cast<std::size_t>(d) * kBins;
        for (uint32_t h = 0; h < kBins; ++h) dd[h] = h <= top ? liveSpec[h] : Cd(0.0);
      }
      // The row: harmonics pre-emphasised, one inverse transform of len_j.
      const double* em = preEmph[j].data();
      std::fill(bufIn.begin(), bufIn.begin() + len, Cd(0.0));
      bufIn[0] = liveSpec[0];
      for (uint32_t h = 1; h <= top; ++h) {
        bufIn[h] = liveSpec[h] * em[h];
        bufIn[len - h] = std::conj(bufIn[h]);
      }
      fftLevel[fftOfLevel[j]].inverse(bufIn.data(), bufOut.data());
      float* row = out.mutableRow(d, j);
      for (uint32_t n = 0; n < len; ++n) row[n] = static_cast<float>(bufOut[n].real());
      for (uint32_t q = 0; q < g; ++q) {
        row[-static_cast<int32_t>(g) + static_cast<int32_t>(q)] = row[len - g + q];
        row[len + q] = row[q];
      }
    }
  }
}

// Frame-independent part of a bake: the breakpoints, the read phases' jets beside them, and every sample's taps.
void FrameTableBuilder::Impl::prepareTaps(const FrameBake& b) {
  if (tapsValid && tapsM == bakeM && tapsFor.warpMode == b.warpMode && tapsFor.warp == b.warp &&
      tapsFor.sync == b.sync)
    return;
  const uint32_t m = bakeM;
  const bool two = b.warpMode == WarpMode::Mirror && b.warp != 0.0f;
  mixB = 0;
  for (uint32_t n = 0; n < m; ++n) {
    const double phi = (n + 0.5) / m;  // half-sample grid: no sample sits on a breakpoint (φ = 0, k/s)
    const auto taps = warpPhase(b.warpMode, syncPhase(phi, b.sync), b.warp);
    mixB = taps.mixB;
    for (int t = 0; t < (two ? 2 : 1); ++t) {
      const double pos = (t == 0 ? taps.a : taps.b) * m;
      const double fi = std::floor(pos);
      tapIdx[t][n] = static_cast<int32_t>(fi) + static_cast<int32_t>(kFrameTableGuard) - 2;
      interpWeights(pos - fi, tapW[t].data() + static_cast<std::size_t>(n) * 6);
    }
  }
  numBp = warpBreakpoints(b.warpMode, b.warp, b.sync, bp, kMaxBreakpoints);
  constexpr double kEta = 1e-9;
  for (uint32_t i = 0; i < numBp; ++i)
    for (int side = 0; side < 2; ++side) {
      double at = bp[i] + (side == 0 ? -kEta : kEta);
      if (at < 0.0) at += 1.0;
      const auto taps = warpPhase(b.warpMode, syncPhase(Jet(at, 1.0, 0.0), b.sync), b.warp);
      bpOmega[i][side][0] = taps.a;
      bpOmega[i][side][1] = taps.b;
    }
  tapsFor = b;
  tapsM = m;
  tapsValid = true;
}

// The value / slope / curvature jumps of one frame's baked cycle at every breakpoint, and the piecewise cubic of
// their Bernoulli-polynomial sum (the correction subtracted before the FFT).
void FrameTableBuilder::Impl::jumps(const FrameBake& b, const Cd* c) {
  constexpr double kEta = 1e-9;
  const bool two = mixB != 0.0;
  auto xJet = [&](const Jet& w) {  // x(ω(φ)) with its φ-derivatives
    double x = c[0].real(), x1 = 0, x2 = 0;
    const Cd step = std::polar(1.0, kTwoPi * w.v);
    Cd e = step;
    for (uint32_t h = 1; h < kBins; ++h, e *= step) {
      const Cd ce = c[h] * e;
      const double hw = kTwoPi * h;
      x += 2.0 * ce.real();
      x1 += -2.0 * hw * ce.imag();     // 2 Re(i·hw·ce)
      x2 += -2.0 * hw * hw * ce.real();
    }
    return Jet(x, x1 * w.d1, x2 * w.d1 * w.d1 + x1 * w.d2);
  };
  for (uint32_t i = 0; i < numBp; ++i) {
    double lim[2][3];
    for (int side = 0; side < 2; ++side) {
      Jet v = xJet(bpOmega[i][side][0]);
      if (two) v = v * Jet(1.0 - mixB) + xJet(bpOmega[i][side][1]) * Jet(mixB);
      const Jet y = foldValue(v, b.fold);
      const double e = side == 0 ? kEta : -kEta;  // extrapolate from φ_b ∓ η to φ_b
      lim[side][0] = y.v + e * y.d1 + 0.5 * e * e * y.d2;
      lim[side][1] = y.d1 + e * y.d2;
      lim[side][2] = y.d2;
    }
    for (int k = 0; k < 3; ++k) corr[i][k] = lim[1][k] - lim[0][k];
  }
  // On piece i = [φ_i, φ_{i+1}), with u = φ − φ_i and f_b = frac(φ − φ_b) = u + δ_b:
  //   C(φ) = Σ_b D0·K1(f_b) + D1·K2(f_b) + D2·K3(f_b),  K1 = ½ − f, K2 = −(f² − f + 1/6)/2, K3 = −(f³ − 1.5f² + ½f)/6
  // (−B̃_n / n!: Fourier coefficients e^(−2πihφ_b) / (2πih)^n, unit jumps in the (n − 1)-th derivative).
  for (uint32_t i = 0; i < numBp; ++i) {
    double* p = piece[i];
    p[0] = p[1] = p[2] = p[3] = 0;
    for (uint32_t k = 0; k < numBp; ++k) {
      const double dl = bp[i] - bp[k] + (k > i ? 1.0 : 0.0);
      const double d0 = corr[k][0], d1 = corr[k][1], d2 = corr[k][2];
      p[0] += d0 * (0.5 - dl) - d1 * (dl * dl - dl + 1.0 / 6.0) / 2.0 -
              d2 * (dl * dl * dl - 1.5 * dl * dl + 0.5 * dl) / 6.0;
      p[1] += -d0 - d1 * (2.0 * dl - 1.0) / 2.0 - d2 * (3.0 * dl * dl - 3.0 * dl + 0.5) / 6.0;
      p[2] += -d1 / 2.0 - d2 * (6.0 * dl - 3.0) / 12.0;
      p[3] += -d2 / 6.0;
    }
  }
}

void FrameTableBuilder::Impl::bakePair(const FrameBake& b, Cd* fa, Cd* fb) {
  prepareTaps(b);
  const uint32_t m = bakeM;
  const bool two = mixB != 0.0;
  // 1. Both frames' cycles at M points (pre-emphasised for the interpolator), one complex inverse FFT.
  std::fill(bufIn.begin(), bufIn.begin() + m, Cd(0.0));
  for (uint32_t h = 0; h < kBins; ++h) {
    const Cd a = fa[h] * bakeEmph[h], bb = fb ? fb[h] * bakeEmph[h] : Cd(0.0);
    if (h == 0) {
      bufIn[0] = Cd(a.real(), bb.real());
      continue;
    }
    bufIn[h] = a + Cd(0.0, 1.0) * bb;
    bufIn[m - h] = std::conj(a) + Cd(0.0, 1.0) * std::conj(bb);
  }
  bakeFft->inverse(bufIn.data(), bufOut.data());
  const uint32_t g = kFrameTableGuard;
  for (uint32_t n = 0; n < m; ++n) {
    xa[g + n] = bufOut[n].real();
    xb[g + n] = bufOut[n].imag();
  }
  for (uint32_t k = 0; k < g; ++k) {
    xa[k] = xa[m + k];
    xb[k] = xb[m + k];
    xa[g + m + k] = xa[g + k];
    xb[g + m + k] = xb[g + k];
  }
  // 2. Sample y = fold(read(x)) − C at (n + ½)/M for each frame.
  Cd* frames[2] = {fa, fb};
  std::vector<double>* xs[2] = {&xa, &xb};
  for (int f = 0; f < 2; ++f) {
    if (!frames[f]) {
      for (uint32_t n = 0; n < m; ++n) bufIn[n].imag(0.0);
      continue;
    }
    jumps(b, frames[f]);
    const double* x = xs[f]->data();
    uint32_t pc = 0;
    for (uint32_t n = 0; n < m; ++n) {
      const double phi = (n + 0.5) / m;
      while (pc + 1 < numBp && phi >= bp[pc + 1]) ++pc;
      const double* wa = tapW[0].data() + static_cast<std::size_t>(n) * 6;
      const double* ya = x + tapIdx[0][n];
      double v = wa[0] * ya[0] + wa[1] * ya[1] + wa[2] * ya[2] + wa[3] * ya[3] + wa[4] * ya[4] + wa[5] * ya[5];
      if (two) {
        const double* wb = tapW[1].data() + static_cast<std::size_t>(n) * 6;
        const double* yb = x + tapIdx[1][n];
        const double vb = wb[0] * yb[0] + wb[1] * yb[1] + wb[2] * yb[2] + wb[3] * yb[3] + wb[4] * yb[4] + wb[5] * yb[5];
        v = v * (1.0 - mixB) + vb * mixB;
      }
      const double* p = piece[pc];
      const double u = phi - bp[pc];
      const double y = foldValue(v, b.fold) - (p[0] + u * (p[1] + u * (p[2] + u * p[3])));
      if (f == 0) bufIn[n] = Cd(y, 0.0);
      else bufIn[n].imag(y);
    }
    std::memcpy(corrPair[f], corr, sizeof(corr));  // step 3 needs both frames' jumps
  }
  // 3. One forward FFT; separate the two real spectra; undo the half-sample shift; add the corrections exactly.
  bakeFft->forward(bufIn.data(), bufOut.data());
  for (int f = 0; f < 2; ++f) {
    if (!frames[f]) continue;
    Cd* c = frames[f];
    const auto& jmp = corrPair[f];
    for (uint32_t h = 0; h < kBins; ++h) {
      const Cd z = bufOut[h], zc = std::conj(bufOut[(m - h) % m]);
      Cd y = f == 0 ? (z + zc) * 0.5 : (z - zc) * Cd(0.0, -0.5);
      y *= std::polar(1.0 / m, -std::numbers::pi * h / m);
      if (h > 0) {
        const Cd i1(0.0, -1.0 / (kTwoPi * h));  // 1 / (2πih)
        const Cd i2 = i1 * i1, i3 = i2 * i1;
        for (uint32_t i = 0; i < numBp; ++i)
          y += std::polar(1.0, -kTwoPi * h * bp[i]) * (jmp[i][0] * i1 + jmp[i][1] * i2 + jmp[i][2] * i3);
      } else {
        y = Cd(y.real(), 0.0);
      }
      c[h] = y;
    }
  }
}

// Eight frames per transform: four lanes of Cd4, each a complex signal carrying two frames (real and imaginary).
void FrameTableBuilder::Impl::levels(FrameTable& out, bool antiderivative) {
  const uint32_t g = kFrameTableGuard;
  // Pass 0: the value rows; pass 1 (Shape mode): the antiderivative's periodic part, c_h / (2πih), DC 0.
  for (int pass = 0; pass < (antiderivative ? 2 : 1); ++pass)
  for (uint32_t s0 = 0; s0 < selCount; s0 += 8) {
    const Cd* c[8];
    for (uint32_t f = 0; f < 8; ++f)
      c[f] = s0 + f < selCount ? dense.data() + static_cast<std::size_t>(sel[s0 + f]) * kBins : nullptr;
    for (uint32_t j = 0; j < layout.levels; ++j) {
      if (j > 0 && layout.offset[j] == layout.offset[j - 1]) continue;
      const uint32_t len = layout.length[j], top = layout.top[j];
      const double* e = preEmph[j].data();
      std::memset(static_cast<void*>(buf4In.data()), 0, sizeof(detail::Cd4) * len);
      for (uint32_t lane = 0; lane < 4; ++lane) {
        const Cd* ca = c[2 * lane];
        const Cd* cb = c[2 * lane + 1];
        if (!ca) break;
        // Bin h of lane L: A + iB; bin len − h: conj(A) + i·conj(B) (A, B: the two frames' pre-emphasised c_h).
        double* z0 = reinterpret_cast<double*>(&buf4In[0]);
        z0[lane] = pass == 0 ? ca[0].real() : 0.0;
        z0[4 + lane] = cb && pass == 0 ? cb[0].real() : 0.0;
        for (uint32_t h = 1; h <= top; ++h) {
          // The antiderivative of c_h·e^(2πihφ) is c_h / (2πih)·e^(2πihφ).
          const Cd s = pass == 0 ? Cd(e[h]) : Cd(0.0, -e[h] / (kTwoPi * h));  // 1 / (i·a) = −i / a
          const Cd A = ca[h] * s, B = cb ? cb[h] * s : Cd(0.0);
          double* zp = reinterpret_cast<double*>(&buf4In[h]);
          double* zn = reinterpret_cast<double*>(&buf4In[len - h]);
          zp[lane] = A.real() - B.imag();
          zp[4 + lane] = A.imag() + B.real();
          zn[lane] = A.real() + B.imag();
          zn[4 + lane] = B.real() - A.imag();
        }
      }
      fftLevel[fftOfLevel[j]].inverse(buf4In.data(), buf4Out.data());
      for (uint32_t f = 0; f < 8 && c[f]; ++f) {
        float* row = pass == 0 ? out.mutableRow(sel[s0 + f], j) : out.mutableAdRow(sel[s0 + f], j);
        const uint32_t k = (f / 2) + (f % 2 ? 4 : 0);  // lane, then real (even f) or imaginary (odd f)
        for (uint32_t n = 0; n < len; ++n) row[n] = static_cast<float>(reinterpret_cast<const double*>(&buf4Out[n])[k]);
        for (uint32_t q = 0; q < g; ++q) {
          row[-static_cast<int32_t>(g) + static_cast<int32_t>(q)] = row[len - g + q];
          row[len + q] = row[q];
        }
      }
    }
  }
}

// TMM.8 (Shape mode): each frame's DC and each level's value at phase 0, as the readers interpolate it (z = −½ at
// index 0; the polynomial is not interpolating, so this is not row[0]).
void FrameTableBuilder::Impl::edges(FrameTable& out) {
  float* dc = out.mutableFrameDc();
  float* edge = out.mutableLevelEdge();
  const uint32_t levels = out.view().levels;
  double w[6];
  interpWeights(0.0, w);
  for (uint32_t si = 0; si < selCount; ++si) {
    const uint32_t d = sel[si];
    dc[d] = static_cast<float>(dense[static_cast<std::size_t>(d) * kBins].real());
    for (uint32_t j = 0; j < levels; ++j) {
      const float* row = out.mutableRow(d, j);
      double v = 0.0;
      for (int q = 0; q < 6; ++q) v += w[q] * row[q - 2];
      edge[static_cast<std::size_t>(d) * levels + j] = static_cast<float>(v);
    }
  }
}

// TMM.12-fix1 (step 6): when every dense frame is DC + harmonic 1 (the RMS of harmonics 2 … 1023 at most
// kFrameSineTol of the frame's peak |c_0| + 2|c_1|, at least 1: a float-rounded sine keyframe holds ≈ 1e-8), its
// coefficients for the readers' sine path: x(φ) = c_0 + 2|c_1|·cos(2πφ + arg c_1) = dc + a·sin(2π(φ + θ)),
// θ = arg c_1 / 2π + ¼ wrapped to [−½, ½) (snapped to 0 within 2^−30: a sine keyframe's float noise).
void FrameTableBuilder::Impl::sines(FrameTable& out) {
  constexpr double kFrameSineTol = 1e-7;
  const uint32_t frames = out.frames();
  for (uint32_t d = 0; d < frames; ++d) {
    const Cd* c = dense.data() + static_cast<std::size_t>(d) * kBins;
    double rest = 0.0;
    for (uint32_t h = 2; h < kBins; ++h) rest += std::norm(c[h]);
    const double peak = std::max(1.0, std::abs(c[0].real()) + 2.0 * std::abs(c[1]));
    if (!(std::sqrt(2.0 * rest) <= kFrameSineTol * peak)) {
      out.setSine(false);
      return;
    }
  }
  float* q = out.mutableSine();
  for (uint32_t d = 0; d < frames; ++d) {
    const Cd* c = dense.data() + static_cast<std::size_t>(d) * kBins;
    const double a = 2.0 * std::abs(c[1]);
    double th = a > 0.0 ? std::arg(c[1]) / kTwoPi + 0.25 : 0.0;
    th -= std::floor(th + 0.5);  // [−½, ½)
    if (std::abs(th) < 0x1p-30) th = 0.0;
    q[4 * d + 0] = static_cast<float>(c[0].real());
    q[4 * d + 1] = static_cast<float>(a);
    q[4 * d + 2] = static_cast<float>(th);
    q[4 * d + 3] = 0.0f;
  }
  out.setSine(true);
}

uint32_t frameBakeResolution(const FrameTableSpec& spec) noexcept {
  const FrameBake& b = spec.bake;
  if (!b.active()) return spec.bakeResolution;
  // Estimated bandwidth of the composed read, in harmonics: 1023 × the read phase's largest slope × the fold's
  // spread (θ, its FM-like index for a full-scale input). The bake needs M ≥ 2·B (the 16× default covers every
  // single control at any value: sync 16×, pulse width 16×, fold θ = 4π).
  double slope = 1.0 + (kSyncMaxRatio - 1.0) * b.sync;
  if (b.warp != 0.0f) {
    if (b.warpMode == WarpMode::Bend) {
      const double c = bendCurve(b.warp);
      slope *= std::max(c, 1.0 / c);
    } else if (b.warpMode == WarpMode::PulseWidth) {
      const double w = pulseWidth(b.warp);
      slope *= 0.5 / std::min(w, 1.0 - w);
    }
  }
  const double spread = std::max(1.0, foldShape(b.fold).theta);
  const double need = 2.0 * kFrameTableMaxHarmonic * slope * spread / kKeyframeCycle;
  uint32_t r = spec.bakeResolution;
  while (r < kMaxAutoBakeResolution && r < need) r *= 2;
  return r;
}

FrameTableBuilder::FrameTableBuilder() : impl_(std::make_unique<Impl>()) {}
FrameTableBuilder::~FrameTableBuilder() = default;

Status FrameTableBuilder::validate(const FrameTableSpec& spec, const FrameTable& out) {
  const auto keys = spec.keyframes.size();
  if (keys < 1 || keys > kMaxKeyframes) return invalid("frame table needs 1 to 256 keyframes");
  if (!spec.gaps.empty() && spec.gaps.size() != keys - 1) return invalid("frame table gaps must be keyframes − 1");
  for (const GapMode m : spec.gaps)
    if (m != GapMode::Spectral && m != GapMode::Crossfade) return invalid("unknown gap mode");
  float prevPos = 0.0f;
  for (std::size_t k = 0; k < keys; ++k) {
    const Keyframe& kf = spec.keyframes[k];
    if (kf.cycle.size() != kKeyframeCycle) return invalid("keyframe cycles must have 2048 samples");
    if (!(kf.position >= prevPos && kf.position <= 1.0f))
      return invalid("keyframe positions must be in [0, 1] and non-decreasing");
    prevPos = kf.position;
    for (const float v : kf.cycle) {
      if (!std::isfinite(v)) return invalid("keyframe cycle has a non-finite sample");
      if (std::fabs(v) > kMaxKeyframeSample) return invalid("keyframe cycle has a sample beyond ±64");
    }
  }
  if (spec.denseFrames < 1 || spec.denseFrames > kFrameTableMaxFrames) return invalid("dense frames must be 1 to 256");
  const FrameBake& b = spec.bake;
  if (!(b.warp >= -1.0f && b.warp <= 1.0f) || !(b.fold >= 0.0f && b.fold <= 1.0f) ||
      !(b.sync >= 0.0f && b.sync <= 1.0f) || static_cast<uint32_t>(b.warpMode) >= kWarpModeCount)
    return invalid("bake values out of range (warp [−1, 1], fold and sync [0, 1])");
  if (spec.bakeResolution < 4 || spec.bakeResolution > 128 || !std::has_single_bit(spec.bakeResolution))
    return invalid("bake resolution must be a power of two in [4, 128]");
  if (out.frames() != spec.denseFrames || out.layout().sampleRate != spec.sampleRate ||
      out.hasAntiderivative() != spec.antiderivative)
    return invalid("frame table was allocated for another frame count, sample rate or mode");
  return Ok{};
}

Status FrameTableBuilder::buildInto(const FrameTableSpec& spec, FrameTable& out) {
  if (auto s = validate(spec, out); !s) return s;
  if (auto s = impl_->ensure(spec); !s) return s;
  for (uint32_t d = 0; d < spec.denseFrames; ++d) impl_->sel[d] = d;
  impl_->selCount = spec.denseFrames;
  return impl_->run(spec, out, true);
}

Status FrameTableBuilder::buildFramesInto(const FrameTableSpec& spec, FrameTable& out, std::span<const uint32_t> frames) {
  if (auto s = validate(spec, out); !s) return s;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i] >= spec.denseFrames) return invalid("buildFramesInto: frame index out of range");
    if (i > 0 && frames[i] <= frames[i - 1]) return invalid("buildFramesInto: frames must be ascending and distinct");
  }
  if (frames.empty()) return Ok{};
  if (auto s = impl_->ensure(spec); !s) return s;
  std::copy(frames.begin(), frames.end(), impl_->sel.begin());
  impl_->selCount = static_cast<uint32_t>(frames.size());
  return impl_->run(spec, out, frames.size() == spec.denseFrames);
}

Status FrameTableBuilder::Impl::run(const FrameTableSpec& spec, FrameTable& out, bool all) {
  keyframeSpectra(spec);
  denseFrames(spec);
  if (spec.liveBand && spec.bake.active() && !spec.antiderivative) {
    // TMM.8-fix: the unbaked frames' levels are the live read's source; each output level is the live read.
    levels(liveSrc, false);
    liveBandLevels(spec, out);
    out.setSine(false);  // the rows are the live read, not the dense spectra
  } else {
    levels(out, spec.antiderivative);
    if (all) sines(out);
    else out.setSine(false);  // TMM.16: the other frames' spectra are not known here (their rows are right: read them)
  }
  if (spec.antiderivative) edges(out);
  lastFrames = spec.denseFrames;
  return Ok{};
}

Result<FrameTable> FrameTableBuilder::build(const FrameTableSpec& spec) {
  auto layout = frameTableLayout(spec.sampleRate);
  if (!layout) return std::move(layout).error();
  if (spec.denseFrames < 1 || spec.denseFrames > kFrameTableMaxFrames) return invalid("dense frames must be 1 to 256");
  auto t = FrameTable::allocate(layout.value(), spec.denseFrames, spec.antiderivative);
  if (!t) return std::move(t).error();
  if (auto s = buildInto(spec, t.value()); !s) return std::move(s).error();
  return t;
}

std::span<const std::complex<double>> FrameTableBuilder::frameSpectrum(uint32_t frame) const noexcept {
  if (frame >= impl_->lastFrames) return {};
  return {impl_->dense.data() + static_cast<std::size_t>(frame) * kBins, kBins};
}

uint32_t FrameTableBuilder::frames() const noexcept { return impl_->lastFrames; }

uint64_t frameTableSpecHash(const FrameTableSpec& spec) noexcept {
  // FNV-1a over 64-bit words, then a final avalanche (splitmix64).
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 0x100000001b3ull;
  };
  auto bits = [](float f) { return static_cast<uint64_t>(std::bit_cast<uint32_t>(f)); };
  mix(std::bit_cast<uint64_t>(spec.sampleRate));
  mix(spec.denseFrames | (static_cast<uint64_t>(spec.bakeResolution) << 32));
  mix(static_cast<uint64_t>(spec.removeDc) | static_cast<uint64_t>(spec.normalise) << 1 |
      static_cast<uint64_t>(spec.antiderivative) << 2 | static_cast<uint64_t>(spec.liveBand) << 3 |
      static_cast<uint64_t>(spec.bake.warpMode) << 8);
  mix(bits(spec.bake.warp) | bits(spec.bake.fold) << 32);
  mix(bits(spec.bake.sync));
  mix(spec.keyframes.size());
  for (std::size_t g = 0; g < spec.gaps.size(); ++g) mix(static_cast<uint64_t>(spec.gaps[g]) + (g << 8));
  for (const Keyframe& k : spec.keyframes) {
    mix(bits(k.position));
    const float* c = k.cycle.data();
    for (std::size_t i = 0; i + 1 < k.cycle.size(); i += 2) mix(bits(c[i]) | bits(c[i + 1]) << 32);
    if (k.cycle.size() % 2) mix(bits(c[k.cycle.size() - 1]));
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

AlignedBuffer<float> basicCycle(BasicCycle shape, uint32_t n, uint64_t seed) {
  AlignedBuffer<float> c(n);
  if (shape == BasicCycle::Noise) {
    uint64_t z = seed * 0x9E3779B97F4A7C15ull + 0x5DEECE66Dull;
    std::vector<double> v(n);
    double mean = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
      z = z * 6364136223846793005ull + 1442695040888963407ull;  // PCG's LCG step
      v[i] = static_cast<double>(z >> 11) / 9007199254740992.0 * 2.0 - 1.0;
      mean += v[i];
    }
    mean /= n;
    double peak = 0.0;
    for (double& x : v) {
      x -= mean;
      peak = std::max(peak, std::fabs(x));
    }
    for (uint32_t i = 0; i < n; ++i) c[i] = static_cast<float>(peak > 0 ? v[i] / peak : 0.0);
    return c;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const double ph = static_cast<double>(i) / n;
    double v = 0;
    switch (shape) {
      case BasicCycle::Sine: v = std::sin(kTwoPi * ph); break;
      case BasicCycle::Triangle: v = ph < 0.25 ? 4.0 * ph : ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0; break;
      case BasicCycle::Saw: v = 2.0 * ph - 1.0; break;
      case BasicCycle::Square: v = ph < 0.5 ? 1.0 : -1.0; break;
      case BasicCycle::Pulse25: v = ph < 0.25 ? 1.0 : -1.0; break;
      case BasicCycle::Noise: break;
    }
    c[i] = static_cast<float>(v);
  }
  return c;
}

}  // namespace pq::dsp
