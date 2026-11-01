#pragma once

// TMM.8 test rig: the Wavetable's live path as an oversample(n) region runs it, at kernel level — the base-rate phase
// up through oversample_phase, wavetable_live at n·sr, oversample_down — and the 64× double-precision reference the
// alias proof measures it against (pq/dsp/wave_live.h: the same formulas and corrections at 64× the rate, in double;
// its spectrum by one complex FFT over a whole period). Both sides see the controls as exact functions of time.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <numbers>
#include <vector>

#include "frame_table_fixtures.h"
#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/dsp/wave_live.h"

namespace wlrig {

using namespace pq::dsp;
using TablePtr = std::shared_ptr<const FrameTable>;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

inline TablePtr build(std::vector<ftfix::Cycle> cycles, double sr, FrameBake bake = {}, bool antiderivative = false,
                      uint32_t dense = 64, bool liveBand = false) {
  FrameTableBuilder b;
  ftfix::SpecHolder s(std::move(cycles), sr, dense);
  s.spec.bake = bake;
  s.spec.antiderivative = antiderivative;
  s.spec.liveBand = liveBand;
  auto t = b.build(s.spec);
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  return std::make_shared<const FrameTable>(std::move(t.value()));
}

// A cycle with harmonics 1 … top of a saw (1/h), every one: band-limited "in band" input for a given pitch.
inline ftfix::Cycle sawTo(uint32_t top) {
  std::vector<std::pair<uint32_t, std::pair<double, double>>> parts;
  for (uint32_t h = 1; h <= top; ++h) parts.push_back({h, {0.6 / h, -0.5 * std::numbers::pi}});
  return ftfix::fromHarmonics(parts);
}

struct State {
  pq::AlignedBuffer<float> mem;
  Atom atom;
  State(Atom a, uint32_t lanes, double sr) : mem(std::max<size_t>(kernelInfo(a).stateSize / 4, 8)), atom(a) {
    StateInit si;
    si.lanes = lanes;
    si.sampleRate = sr;
    initState(a, mem.data(), si);
  }
};

// Controls as functions of time t in base samples (the rig evaluates them per oversampled sample).
using Ctl = std::function<double(double)>;
inline Ctl constant(double v) {
  return [v](double) { return v; };
}
inline Ctl sine(double depth, double centre, double cyclesPerSample, double phase0 = 0.3) {
  return [=](double t) { return centre + depth * std::sin(kTwoPi * cyclesPerSample * t + phase0); };
}

struct LiveRegion {
  uint32_t n;
  double sr;
  WarpMode mode;
  bool corr;
  Ctl warp, fold, sync;
  bool splControls = false;  // controls as Spl patterns (one value per block, at its end)
  State up{Atom::OversamplePhase, 1, 48000.0}, down{Atom::OversampleDown, 1, 48000.0};
  State live;
  KernelFn fUp = findKernel(Atom::OversamplePhase, 1, 2u), fDown = findKernel(Atom::OversampleDown, 1, 2u);
  int64_t time = 0;  // base samples processed

  LiveRegion(uint32_t factor, double rate, WarpMode m, bool c, Ctl w, Ctl f, Ctl s)
      : n(factor), sr(rate), mode(m), corr(c), warp(std::move(w)), fold(std::move(f)), sync(std::move(s)),
        live(Atom::WavetableLive, 1, rate * factor) {}

  KernelFn liveFn(uint32_t spl) const { return findKernel(Atom::WavetableLive, 1, spl); }

  // `frames` base samples of phase → out.
  void block(const FrameTableSlot& slot, const float* phase, uint32_t frames, float* out) {
    const uint32_t m = frames * n;
    std::vector<float> ph(m), w(m), f(m), s(m), y(m);
    alignas(32) float factor[8];
    std::fill(factor, factor + 8, static_cast<float>(n));
    {
      const float* ins[2] = {phase, factor};
      float* outs[1] = {ph.data()};
      KernelArgs a{ins, outs, up.mem.data(), nullptr, n == 1 ? frames : m, 2, 1, 2u};
      if (n > 1) fUp(a);
      else std::copy(phase, phase + frames, ph.begin());
    }
    alignas(32) float pos[8] = {}, wv[8], fv[8], sv[8], modeV[8], corrV[8];
    std::fill(modeV, modeV + 8, static_cast<float>(mode));
    std::fill(corrV, corrV + 8, corr ? 1.0f : 0.0f);
    uint32_t spl = (1u << 1) | (1u << 5) | (1u << 6);
    if (splControls) {
      const double tEnd = static_cast<double>(time + frames) - 1.0;
      std::fill(wv, wv + 8, static_cast<float>(warp(tEnd)));
      std::fill(fv, fv + 8, static_cast<float>(fold(tEnd)));
      std::fill(sv, sv + 8, static_cast<float>(sync(tEnd)));
      spl |= (1u << 2) | (1u << 3) | (1u << 4);
    } else {
      for (uint32_t k = 0; k < frames; ++k)
        for (uint32_t j = 0; j < n; ++j) {
          // Sub-sample j of base frame k sits at k − (n − 1 − j)/n (the phase interpolation's timing).
          const double t = static_cast<double>(time + k) - static_cast<double>(n - 1 - j) / n;
          w[k * n + j] = static_cast<float>(warp(t));
          f[k * n + j] = static_cast<float>(fold(t));
          s[k * n + j] = static_cast<float>(sync(t));
        }
    }
    {
      const float* ins[7] = {ph.data(), pos, splControls ? wv : w.data(), splControls ? fv : f.data(),
                             splControls ? sv : s.data(), modeV, corrV};
      float* outs[1] = {y.data()};
      KernelArgs a{ins, outs, live.mem.data(), &slot, m, 7, 1, spl};
      const KernelFn fn = liveFn(spl);
      REQUIRE(fn != nullptr);
      fn(a);
    }
    if (n == 1) {
      std::copy(y.begin(), y.end(), out);
    } else {
      const float* ins[2] = {y.data(), factor};
      float* outs[1] = {out};
      KernelArgs a{ins, outs, down.mem.data(), nullptr, m, 2, 1, 2u};
      fDown(a);
    }
    time += frames;
  }
};

// Shape mode as a region runs it: x (audio) up through the half-bands, wavetable_shape at n·sr, down.
struct ShapeRegion {
  uint32_t n;
  double sr;
  State up{Atom::OversampleUp, 1, 48000.0}, down{Atom::OversampleDown, 1, 48000.0};
  State shape;
  KernelFn fUp = findKernel(Atom::OversampleUp, 1, 2u), fDown = findKernel(Atom::OversampleDown, 1, 2u);
  KernelFn fShape = findKernel(Atom::WavetableShape, 1, 2u);
  ShapeRegion(uint32_t factor, double rate) : n(factor), sr(rate), shape(Atom::WavetableShape, 1, rate * factor) {}
  void block(const FrameTableSlot& slot, const float* x, uint32_t frames, float* out) {
    const uint32_t m = frames * n;
    std::vector<float> xu(m), y(m);
    alignas(32) float factor[8];
    std::fill(factor, factor + 8, static_cast<float>(n));
    if (n > 1) {
      const float* ins[2] = {x, factor};
      float* outs[1] = {xu.data()};
      KernelArgs a{ins, outs, up.mem.data(), nullptr, m, 2, 1, 2u};
      fUp(a);
    } else {
      std::copy(x, x + frames, xu.begin());
    }
    alignas(32) float pos[8] = {};
    {
      const float* ins[2] = {xu.data(), pos};
      float* outs[1] = {y.data()};
      KernelArgs a{ins, outs, shape.mem.data(), &slot, m, 2, 1, 2u};
      fShape(a);
    }
    if (n > 1) {
      const float* ins[2] = {y.data(), factor};
      float* outs[1] = {out};
      KernelArgs a{ins, outs, down.mem.data(), nullptr, m, 2, 1, 2u};
      fDown(a);
    } else {
      std::copy(y.begin(), y.end(), out);
    }
  }
};

// N base samples of Shape mode's output for x = amp·sin(2π·k0·t/N) after `lead` samples.
inline std::vector<float> renderShape(ShapeRegion& r, const FrameTableSlot& slot, double amp, uint32_t k0, uint32_t N,
                                      uint32_t lead = 16384, uint32_t block = 64) {
  std::vector<float> out, x(block), y(block);
  out.reserve(N);
  uint64_t t = 0;
  while (out.size() < N) {
    for (uint32_t i = 0; i < block; ++i, ++t)
      x[i] = static_cast<float>(amp * std::sin(kTwoPi * static_cast<double>((t * k0) % N) / N));
    r.block(slot, x.data(), block, y.data());
    if (t > lead) out.insert(out.end(), y.begin(), y.end());
  }
  out.resize(N);
  return out;
}

// Bin-exact phase ramp: φ[k] = frac(k·k0 / N) (exact in float).
inline void rampPhase(int64_t from, uint32_t frames, uint32_t k0, uint32_t N, float* out) {
  for (uint32_t i = 0; i < frames; ++i)
    out[i] = static_cast<float>(static_cast<double>((static_cast<uint64_t>(from + i) * k0) % N) / N);
}

// N base samples of the region's output after `lead` base samples (blocks of `block`).
inline std::vector<float> renderLive(LiveRegion& r, const FrameTableSlot& slot, uint32_t k0, uint32_t N,
                                     uint32_t lead = 8192, uint32_t block = 64) {
  std::vector<float> out;
  out.reserve(N);
  std::vector<float> ph(block), y(block);
  uint32_t done = 0;
  while (out.size() < N) {
    rampPhase(r.time, block, k0, N, ph.data());
    r.block(slot, ph.data(), block, y.data());
    if (done >= lead) out.insert(out.end(), y.begin(), y.end());
    done += block;
  }
  out.resize(N);
  return out;
}

// Amplitudes (peak, full-scale sine = 1) of bins 0 … N/2 of an N-sample float signal.
inline std::vector<double> amplitudes(const std::vector<float>& x) {
  const uint32_t N = static_cast<uint32_t>(x.size());
  RealFft fft(N);
  std::vector<float> re(N / 2 + 1), im(N / 2 + 1);
  fft.forward(x.data(), re.data(), im.data());
  std::vector<double> a(N / 2 + 1);
  for (uint32_t b = 0; b <= N / 2; ++b)
    a[b] = (b == 0 ? 1.0 : 2.0) * std::hypot(static_cast<double>(re[b]), static_cast<double>(im[b])) / N;
  return a;
}

// The 64× reference: y(t) on t = i / R (i < N·R) from the same formulas in double, the reference's own corrections
// for every jump and kink (exact event times), and its bins 0 … N/2 (only those below `limitHz` are needed).
// The table is read at the level pair the live path uses at `os` (its speed per oversampled sample: the reference's
// increment × R / os), so the reference is the continuous-time signal the region samples, and every difference below
// 20 kHz is aliasing (or rounding). (With the reference's own speeds it would keep harmonics the region cannot hold
// without aliasing: the in-band difference that makes is the table's band limit at the region's rate, which the
// baked-agreement test reports.)
struct Reference {
  uint32_t R = 64;
  uint32_t os = 4;
  bool corrections = true;
  std::vector<double> amplitudes(const FrameTableView& t, WarpMode mode, const Ctl& warp, const Ctl& fold,
                                 const Ctl& sync, uint32_t k0, uint32_t N) const {
    const size_t M = static_cast<size_t>(N) * R;
    std::vector<double> y(M), im(M, 0.0);
    const double inc = static_cast<double>(k0) / N / R;  // φ per reference sample
    const double incBase = inc * R;                       // φ per base sample
    const double incRep = liveRepIncrement(t, static_cast<float>(incBase));  // the base level's representative
    auto phiAt = [&](size_t i) {
      const double p = static_cast<double>((static_cast<uint64_t>(i) * k0) % (static_cast<uint64_t>(N) * R));
      return p / (static_cast<double>(N) * R);
    };
    // d/dt per reference sample of a control (central difference, in base samples).
    auto rateOf = [&](const Ctl& c, double tb) {
      constexpr double h = 1e-3;
      return (c(tb + h) - c(tb - h)) / (2.0 * h) / R;
    };
    auto stamp = [&](size_t i, double d, double jump, double kink) {
      if (!(d >= 0.0)) return;
      for (int k = -static_cast<int>(kLiveCorrHalf); k < static_cast<int>(kLiveCorrHalf); ++k) {
        double r1, r2;
        liveResidual(k, std::min(d, 1.0), &r1, &r2);
        y[(i + static_cast<size_t>(k + static_cast<int>(M))) % M] += jump * r1 + kink * r2;
      }
    };
    struct Sample {
      LiveControls c;
      double phi = 0, s = 1, psi = 0, sub = 0, psiRate = 0, dwRate = 0, scale = 1;
    };
    auto sampleAt = [&](size_t i) {
      Sample q;
      const double tb = static_cast<double>(i) / R;
      const double w = warp(tb), sy = sync(tb);
      q.c = LiveControls{mode, std::clamp(w, -1.0, 1.0), fold(tb), std::clamp(sy, 0.0, 1.0)};
      q.phi = phiAt(i);
      q.s = 1.0 + 15.0 * q.c.sync;
      const double ds = (sy > 0.0 && sy < 1.0) ? 15.0 * rateOf(sync, tb) : 0.0;
      q.dwRate = (w > -1.0 && w < 1.0) ? rateOf(warp, tb) : 0.0;
      const double prod = q.phi * q.s;
      q.sub = std::floor(prod);
      q.psi = prod - q.sub;
      q.psiRate = inc * q.s + q.phi * ds;  // ψ per reference sample
      q.scale = liveSpeedScale(q.c.fold);
      return q;
    };
    // The taps' level speeds at ψ (side: the branch at a breakpoint), per oversampled sample, scaled.
    auto speedAt = [&](const Sample& q, double psi, int side, double& sa, double& sb) {
      const LiveTaps tp = liveTaps(mode, psi, q.c.warp, side);
      // Base-rate speeds (wave_live.h "Level choice"): ψ′ = e·s + φ·Δs per base sample.
      const double rateRep = incRep * q.s + (q.psiRate - inc * q.s) * R;
      sa = std::fabs(tp.da * rateRep + tp.dadw * q.dwRate * R) * q.scale;
      sb = std::fabs(rateRep) * q.scale;
    };
    Sample prev = sampleAt(M - 1);
    for (size_t i = 0; i < M; ++i) {
      const Sample q = sampleAt(i);
      double sa = 0, sb = 0;
      speedAt(q, q.psi, 0, sa, sb);
      y[i] += liveValueAtPsi(t, 0.0f, q.c, q.psi, incBase, 0, nullptr, sa, sb);
      if (corrections && q.psiRate > 0.0) {
        const double rate = q.psiRate;
        // One-sided values and slopes (per reference sample) at a breakpoint, each with its side's level speeds.
        auto jumpAt = [&](double l, double r, double& j) {
          double gl = 0, gr = 0, la = 0, lb = 0, ra = 0, rb = 0;
          speedAt(q, l, -1, la, lb);
          speedAt(q, r, +1, ra, rb);
          const double yl = liveValueAtPsi(t, 0.0f, q.c, l, incBase, -1, &gl, la, lb);
          const double yr = liveValueAtPsi(t, 0.0f, q.c, r, incBase, +1, &gr, ra, rb);
          j = yr - yl;
          return (gr - gl) * rate;
        };
        if (mode == WarpMode::Mirror && (q.c.warp > 0.0) != (prev.c.warp > 0.0)) {
          const double dw = q.c.warp - prev.c.warp;
          stamp(i, q.c.warp / dw, 0.0, dw * liveMirrorCrossing(t, 0.0f, q.c, q.psi, q.c.warp > 0.0, sa, sb));
        }
        double kinkAt = -1;
        if (mode == WarpMode::Mirror && q.c.warp != 0.0) kinkAt = 0.5;
        if (mode == WarpMode::PulseWidth && q.c.warp != 0.0) kinkAt = 0.5 * (1.0 - (15.0 / 16.0) * q.c.warp);
        double j = 0;
        const double psi = q.psi, prevPsi = prev.psi;
        if (q.phi < prev.phi) {
          const double d = q.phi / inc;
          double end = q.s - std::floor(q.s);
          if (end == 0.0) end = 1.0;
          const double kk = jumpAt(end, 0.0, j);
          stamp(i, d, j, kk);
          if (kinkAt > 0 && psi >= kinkAt) {
            const double k2 = jumpAt(kinkAt, kinkAt, j);
            stamp(i, (psi - kinkAt) / rate, j, k2);
          }
          if (kinkAt > 0 && prevPsi < kinkAt && kinkAt <= end) {
            const double k2 = jumpAt(kinkAt, kinkAt, j);
            stamp(i, d + (end - kinkAt) / rate, j, k2);
          }
        } else if (q.sub > prev.sub) {
          const double kk = jumpAt(1.0, 0.0, j);
          stamp(i, psi / rate, j, kk);
          if (kinkAt > 0 && psi >= kinkAt) {
            const double k2 = jumpAt(kinkAt, kinkAt, j);
            stamp(i, (psi - kinkAt) / rate, j, k2);
          }
          if (kinkAt > 0 && prevPsi < kinkAt) {
            const double k2 = jumpAt(kinkAt, kinkAt, j);
            stamp(i, (psi + 1.0 - kinkAt) / rate, j, k2);
          }
        } else if (q.sub == prev.sub && kinkAt > 0 && prevPsi < kinkAt && psi >= kinkAt) {
          const double k2 = jumpAt(kinkAt, kinkAt, j);
          stamp(i, (psi - kinkAt) / rate, j, k2);
        }
      }
      prev = q;
    }
    fftInPlace(y, im, false);
    std::vector<double> a(N / 2 + 1);
    for (uint32_t b = 0; b <= N / 2; ++b) a[b] = (b == 0 ? 1.0 : 2.0) * std::hypot(y[b], im[b]) / static_cast<double>(M);
    // What sampling this signal at os·sr folds below `limitHz` (the region's irreducible aliasing for these controls):
    // per base bin b, the RSS of the bins j·N·os ± b of the 64× spectrum.
    inherentDb = -400;
    const size_t period = static_cast<size_t>(N) * os;
    for (uint32_t b = 1; b < N / 2 && b * sampleRate / N < limitHz; ++b) {
      double e = 0;
      for (size_t j = period; j < M / 2 + period; j += period)
        for (const size_t k : {j - b, j + b})
          if (k < M / 2 && k > 0) e += (y[k] * y[k] + im[k] * im[k]);
      inherentDb = std::max(inherentDb, 20.0 * std::log10(std::max(2.0 * std::sqrt(e) / static_cast<double>(M), 1e-30)));
    }
    return a;
  }
  double sampleRate = 48000.0, limitHz = 20000.0;
  mutable double inherentDb = -400;
};

struct Compare {
  double worstDb = -400, worstHz = 0;  // largest | |live| − |ref| | below the limit
  double emptyDb = -400;               // the loudest live bin where the reference is below −140 dBFS
};
inline Compare compare(const std::vector<double>& live, const std::vector<double>& ref, double sr, uint32_t N,
                       double limitHz = 20000.0) {
  Compare c;
  for (uint32_t b = 1; b < live.size(); ++b) {
    const double hz = b * sr / N;
    if (hz >= limitHz) break;
    const double e = 20.0 * std::log10(std::max(std::fabs(live[b] - ref[b]), 1e-30));
    if (e > c.worstDb) {
      c.worstDb = e;
      c.worstHz = hz;
    }
    if (ref[b] < 1e-7) c.emptyDb = std::max(c.emptyDb, 20.0 * std::log10(std::max(live[b], 1e-30)));
  }
  return c;
}

}  // namespace wlrig
