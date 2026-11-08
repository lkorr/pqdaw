// EQ.1: the svf atom (pq/dsp/svf_design.h, kernels_svf.cpp).
//   response   the kernel's impulse response, transformed (a DFT in double at 96 log-spaced frequencies), equals
//              svfResponse of the design within 0.01 dB / 0.1° for every mode, stages {1, 2, 8, 64}, both prewarps and
//              cutoffs 20 Hz … 0.45·sr (the full sweep is slow; 16 and 17 stages: the last float and the first double
//              cascade), wherever |H| is within 60 dB of its maximum (below
//              that the float recursion's rounding noise, not the design, sets the measured value)
//   lanes      8 lanes with eight different filters: each lane its own response, bit-identical to the 1-lane kernel
//   matched    a matched bell at 15 kHz / 48 kHz within 0.5 dB of its analog prototype over 0 … 20 kHz (the bilinear
//              one is not: the cramping the matched design removes)
//   design     every mode / stages / extreme input gives finite coefficients and stable poles
//   behaviour  audio-rate cutoff / q / gain sweeps stay bounded and finite; any split of the frames into calls gives
//              the same bits (param and audio rate); a NaN input does not poison the filter; RtScope on the process
//              path (designs at param and audio rate, 1 and 8 lanes)
//   EQ.1b      20 ms param ramps (interpolated coefficients) within −90 dB of a design per frame, no clicks, 8 lanes = 1
//              lane bit for bit (a group on one ramp designs once); after a ramp the exact design runs (bit-identical
//              to a filter set there from the start); the state is sized by its sections; RtScope on the new paths
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/svf_design.h"

using namespace pq::dsp;
using cd = std::complex<double>;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = std::numbers::pi;
constexpr const char* kModeNames[] = {"lowpass", "highpass", "bandpass", "notch", "bell", "lowshelf", "highshelf", "allpass"};

// The atom's input values for one filter.
struct Filt {
  float cutoff = 1000.0f, q = 0.70710678f, gain = 1.0f;
  uint32_t mode = 0, stages = 1, prewarp = 0;
  SvfParams params(double sr = kSr) const {
    return svfParamsFromInputs(cutoff, q, gain, static_cast<float>(mode), static_cast<float>(stages),
                               static_cast<float>(prewarp), sr);
  }
  std::string name() const {
    return std::string(kModeNames[mode]) + " stages " + std::to_string(stages) + (prewarp ? " bilinear" : " matched") +
           " fc " + std::to_string(cutoff) + " q " + std::to_string(q) + " gain " + std::to_string(gain);
  }
};

// The kernel with its state and input buffers; x and every other input can be a block or a pattern per call.
struct Rig {
  uint32_t lanes;
  pq::AlignedBuffer<float> state{sizeof(SvfState) / sizeof(float)};
  pq::AlignedBuffer<float> in[7];
  pq::AlignedBuffer<float> out;
  explicit Rig(uint32_t l, uint32_t maxFrames = 4096, double sr = kSr) : lanes(l), out(static_cast<size_t>(maxFrames) * 8) {
    for (auto& b : in) b = pq::AlignedBuffer<float>(static_cast<size_t>(maxFrames) * 8);
    StateInit init;
    init.lanes = l;
    init.sampleRate = sr;
    initState(Atom::Svf, state.data(), init);
  }
  // Consts and (when spl) cutoff / q / gain as patterns from per-lane filters.
  void setPatterns(const std::vector<Filt>& f) {
    for (uint32_t l = 0; l < 8; ++l) {
      const Filt& g = f[std::min<size_t>(l, f.size() - 1)];
      in[1][l] = g.cutoff;
      in[2][l] = g.q;
      in[3][l] = g.gain;
      in[4][l] = static_cast<float>(g.mode);
      in[5][l] = static_cast<float>(g.stages);
      in[6][l] = static_cast<float>(g.prewarp);
    }
  }
  // One call over `frames`; inputs whose bit is in `blkMask` (bit i = input i) are blocks already filled.
  void call(uint32_t frames, uint32_t splMask) {
    const float* ins[7];
    for (uint32_t i = 0; i < 7; ++i) ins[i] = in[i].data();
    float* outs[1] = {out.data()};
    const KernelFn fn = findKernel(Atom::Svf, lanes, splMask);
    REQUIRE(fn != nullptr);
    const KernelArgs a{ins, outs, state.data(), nullptr, frames, 7, 1, splMask};
    fn(a);
  }
};

constexpr uint32_t kAllSpl = 0x7Eu;  // everything but x a pattern

// The 1-lane impulse response, run in 64-frame calls until a 4096-sample window holds nothing above 1e-12 of the
// peak (at most 2^21 samples).
std::vector<double> impulse(const Filt& f, double sr = kSr) {
  pq::DenormalGuard dg;
  Rig r(1, 64, sr);
  r.setPatterns({f});
  std::vector<double> h;
  double peak = 0.0, window = 0.0;
  for (uint32_t t0 = 0; t0 < (1u << 21); t0 += 64) {
    for (uint32_t t = 0; t < 64; ++t) r.in[0][t] = t0 + t == 0 ? 1.0f : 0.0f;
    r.call(64, kAllSpl);
    for (uint32_t t = 0; t < 64; ++t) {
      const double y = r.out[t];
      h.push_back(y);
      peak = std::max(peak, std::fabs(y));
      window = std::max(window, std::fabs(y));
    }
    if ((t0 + 64) % 4096 == 0) {
      if (h.size() >= 8192 && peak > 0.0 && window <= 1e-12 * peak) break;
      window = 0.0;
    }
  }
  return h;
}

cd dft(const std::vector<double>& h, double w) {
  const cd rot = std::polar(1.0, -w);
  cd z(1.0, 0.0), acc(0.0, 0.0);
  for (size_t n = 0; n < h.size(); ++n) {
    acc += h[n] * z;
    z *= rot;
    if ((n & 4095) == 4095) z = std::polar(1.0, -w * static_cast<double>(n + 1));  // re-anchor the rotation
  }
  return acc;
}

// 96 log-spaced frequencies from 10 Hz to 0.49·sr, and 64 more within half an octave of the cutoff (a deep cascade's
// passband can lie between the coarse points).
std::vector<double> grid(double fc, double sr = kSr) {
  std::vector<double> w;
  const double lo = std::log(10.0), hi = std::log(0.49 * sr);
  for (int i = 0; i < 96; ++i) w.push_back(2.0 * kPi * std::exp(lo + (hi - lo) * i / 95.0) / sr);
  const double a = std::log(fc * std::sqrt(0.5)), b = std::log(std::min(fc * std::sqrt(2.0), 0.49 * sr));
  for (int i = 0; i < 64; ++i) w.push_back(2.0 * kPi * std::exp(a + (b - a) * i / 63.0) / sr);
  return w;
}

struct Err {
  double db = 0.0, deg = 0.0;
  int compared = 0;
};

// Measured (impulse) vs svfResponse on the grid, where |H| is within 60 dB of its maximum on the grid.
Err measure(const Filt& f, double sr = kSr) {
  const std::vector<double> h = impulse(f, sr);
  const SvfParams p = f.params(sr);
  const std::vector<double> w = grid(f.cutoff, sr);
  std::vector<cd> want;
  double top = 0.0;
  for (double x : w) {
    want.push_back(svfResponse(p, x));
    top = std::max(top, std::abs(want.back()));
  }
  Err e;
  for (size_t i = 0; i < w.size(); ++i) {
    if (std::abs(want[i]) < top * 1e-3) continue;
    const cd got = dft(h, w[i]);
    const cd ratio = got / want[i];
    e.db = std::max(e.db, std::fabs(20.0 * std::log10(std::abs(ratio))));
    e.deg = std::max(e.deg, std::fabs(std::arg(ratio)) * 180.0 / kPi);
    ++e.compared;
  }
  return e;
}

Filt filt(uint32_t mode, uint32_t stages, uint32_t prewarp, float fc, bool cut = false) {
  Filt f;
  f.mode = mode;
  f.stages = stages;
  f.prewarp = prewarp;
  f.cutoff = fc;
  const auto m = static_cast<SvfMode>(mode);
  f.q = m == SvfMode::Lowpass || m == SvfMode::Highpass || m == SvfMode::LowShelf || m == SvfMode::HighShelf ? 0.70710678f
                                                                                                           : 1.5f;
  f.gain = static_cast<float>(std::pow(10.0, (cut ? -9.0 : 9.0) / 20.0));
  return f;
}

void sweep(const std::vector<uint32_t>& stagesList, const std::vector<float>& cutoffs) {
  for (uint32_t mode = 0; mode < kSvfModeCount; ++mode)
    for (uint32_t prewarp : {0u, 1u})
      for (uint32_t stages : stagesList)
        for (float fc : cutoffs)
          for (bool cut : {false, true}) {
            const auto m = static_cast<SvfMode>(mode);
            const bool gainMode = m == SvfMode::Bell || m == SvfMode::LowShelf || m == SvfMode::HighShelf;
            if (cut && !gainMode) continue;
            if ((m == SvfMode::Bell || m == SvfMode::Notch) && stages != stagesList.front()) continue;  // always 1
            const Filt f = filt(mode, stages, prewarp, fc, cut);
            INFO(f.name());
            const Err e = measure(f);
            CHECK(e.compared >= 4);
            CHECK(e.db <= 0.01);
            CHECK(e.deg <= 0.1);
          }
}

}  // namespace

TEST_CASE("svf: the kernel's impulse response is svfResponse (every mode, stages 1 / 2 / 8, 1 kHz and 15 kHz)") {
  sweep({1, 2, 8}, {1000.0f, 15000.0f});
}

TEST_CASE("svf: the kernel's impulse response is svfResponse (every mode, stages 1 / 2 / 8 / 16 / 17 / 64, 20 Hz … 0.45·sr)" *
          doctest::test_suite("slow")) {
  sweep({1, 2, 8, 16, 17, 64}, {20.0f, 60.0f, 200.0f, 1000.0f, 5000.0f, 15000.0f, static_cast<float>(0.45 * kSr)});
}

TEST_CASE("svf: 8 lanes, eight filters: each lane its own response, bit-identical to the 1-lane kernel") {
  pq::DenormalGuard dg;
  // Group 0: float cascades (one lane deeper than the others: the rest run identity sections). Group 1: double ones
  // (every lane ≥ 17 sections; a group runs in double when its deepest lane does).
  for (int group = 0; group < 2; ++group) {
    INFO("group " << group);
    std::vector<Filt> f;
    for (uint32_t l = 0; l < 8; ++l)
      f.push_back(filt(l, group == 0 ? 1 + (l % 3) * 3 : 17 + l * 6, l & 1, 300.0f * static_cast<float>(l + 1), l == 4));
    if (group == 0) f[6].stages = 16;
    for (Filt& g : f)  // bell and notch are one section whatever their stages: give them a deep lowpass in group 1
      if (group == 1 && (g.mode == 3 || g.mode == 4)) g.mode = 0;
    Rig r8(8, 64);
    r8.setPatterns(f);
    std::vector<Rig> r1;
    for (uint32_t l = 0; l < 8; ++l) {
      r1.emplace_back(1, 64);
      r1.back().setPatterns({f[l]});
    }
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    bool same = true;
    for (int blk = 0; blk < 40; ++blk) {
      for (uint32_t t = 0; t < 64; ++t)
        for (uint32_t l = 0; l < 8; ++l) {
          const float x = u(rng);
          r8.in[0][t * 8 + l] = x;
          r1[l].in[0][t] = x;
        }
      r8.call(64, kAllSpl);
      for (uint32_t l = 0; l < 8; ++l) {
        r1[l].call(64, kAllSpl);
        for (uint32_t t = 0; t < 64; ++t) same &= std::memcmp(&r8.out[t * 8 + l], &r1[l].out[t], 4) == 0;
      }
    }
    CHECK(same);
    for (uint32_t l = 0; l < 8; ++l) {  // and each 1-lane run is its design (the response check above, per filter)
      INFO(f[l].name());
      const Err e = measure(f[l]);
      CHECK(e.db <= 0.01);
      CHECK(e.deg <= 0.1);
    }
  }
}

TEST_CASE("svf: a matched bell at 15 kHz / 48 kHz is within 0.5 dB of its analog prototype over 0 … 20 kHz") {
  for (float q : {0.70710678f, 2.0f, 5.0f})
    for (double db : {-12.0, -6.0, 6.0, 12.0}) {
      Filt f;
      f.mode = static_cast<uint32_t>(SvfMode::Bell);
      f.cutoff = 15000.0f;
      f.q = q;
      f.gain = static_cast<float>(std::pow(10.0, db / 20.0));
      double worstMatched = 0.0, worstBilinear = 0.0;
      for (uint32_t prewarp : {0u, 1u}) {
        f.prewarp = prewarp;
        const SvfParams p = f.params();
        for (int i = 0; i <= 400; ++i) {
          const double w = 2.0 * kPi * (20000.0 * i / 400.0) / kSr;
          const double e = std::fabs(20.0 * std::log10(std::abs(svfResponse(p, w)) / std::abs(svfAnalogResponse(p, w))));
          (prewarp ? worstBilinear : worstMatched) = std::max(prewarp ? worstBilinear : worstMatched, e);
        }
      }
      INFO("q " << q << " gain " << db << " dB: matched " << worstMatched << " dB, bilinear " << worstBilinear << " dB");
      CHECK(worstMatched <= 0.5);
      CHECK(worstBilinear > 1.0);  // what the matched design removes
    }
}

TEST_CASE("svf: every mode, stages and extreme input designs finite coefficients and stable poles") {
  std::vector<float> cutoffs = {-5.0f, 0.0f, 1e-3f, 0.5f, 7.0f, 20.0f, 76.0f, 77.0f, 1000.0f, 20000.0f, 23520.0f, 24000.0f, 1e9f,
                                std::nanf("")};
  std::vector<float> qs = {0.0f, 0.01f, 0.5f, 0.70710678f, 3.0f, 40.0f, 100.0f, 1e6f, std::nanf("")};
  std::vector<float> gains = {0.0f, 1e-7f, 0.25f, 1.0f, 4.0f, 1e7f, std::nanf("")};
  uint32_t designs = 0;
  bool ok = true;
  std::string firstBad;
  for (uint32_t mode = 0; mode < kSvfModeCount; ++mode)
    for (uint32_t prewarp : {0u, 1u})
      for (uint32_t stages : {1u, 3u, 64u})
        for (float c : cutoffs)
          for (float q : qs)
            for (float g : gains) {
              const SvfParams p = svfParamsFromInputs(c, q, g, static_cast<float>(mode), static_cast<float>(stages),
                                                      static_cast<float>(prewarp), kSr);
              SvfSection s[kSvfMaxStages];
              const uint32_t n = svfDesign(p, s);
              for (uint32_t k = 0; k < n; ++k) {
                const SvfKernelCoefs kc = svfKernelCoefs(s[k]);
                const double v[] = {kc.d1, kc.b1, kc.b2, kc.m0, kc.h1, kc.h2};
                bool good = std::all_of(std::begin(v), std::end(v), [](double x) { return std::isfinite(x); });
                // The recursion's matrix [[1 + d1, −b1], [b1, 1 − b2]]: both eigenvalues inside the unit circle (Jury:
                // det < 1, 1 + det ± trace > 0), written without the cancellation of 1 + det − trace near DC.
                const double d1 = kc.d1, b1 = kc.b1, b2 = kc.b2;
                const bool identity = d1 == 0.0 && b1 == 0.0 && b2 == 0.0;  // no recursion
                good &= identity || (d1 - b2 - d1 * b2 + b1 * b1 < 0.0 && b1 * b1 - d1 * b2 > 0.0 &&
                                     4.0 + 2.0 * d1 - 2.0 * b2 - d1 * b2 + b1 * b1 > 0.0);
                if (!good && ok) {
                  ok = false;
                  firstBad = std::string(kModeNames[mode]) + " prewarp " + std::to_string(prewarp) + " stages " +
                             std::to_string(stages) + " c " + std::to_string(c) + " q " + std::to_string(q) + " g " +
                             std::to_string(g) + " section " + std::to_string(k);
                }
              }
              ++designs;
            }
  INFO(firstBad);
  CHECK(ok);
  CHECK(designs > 10000);
}

namespace {
// 12000 samples (0.25 s) with the cutoff swept over 20 Hz … 0.49·sr at 28 Hz, q and gain at 12 / 20 Hz.
void audioRateSweep(std::initializer_list<uint32_t> stagesList) {
  pq::DenormalGuard dg;
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (uint32_t mode = 0; mode < kSvfModeCount; ++mode)
    for (uint32_t prewarp : {0u, 1u})
      for (uint32_t stages : stagesList) {
        Rig r(1, 64);
        Filt f;
        f.mode = mode;
        f.prewarp = prewarp;
        f.stages = stages;
        r.setPatterns({f});
        double worst = 0.0;
        bool finite = true;
        for (uint32_t t0 = 0; t0 < 12000; t0 += 64) {
          for (uint32_t t = 0; t < 64; ++t) {
            const double n = t0 + t;
            r.in[0][t] = u(rng);
            r.in[1][t] = static_cast<float>(20.0 * std::pow(0.49 * kSr / 20.0, 0.5 + 0.5 * std::sin(n * 2.0 * kPi * 28.0 / kSr)));
            r.in[2][t] = static_cast<float>(0.5 + 0.4 * std::sin(n * 2.0 * kPi * 12.0 / kSr));
            r.in[3][t] = static_cast<float>(std::pow(10.0, 18.0 * std::sin(n * 2.0 * kPi * 20.0 / kSr) / 20.0));
          }
          r.call(64, 0x70u);
          for (uint32_t t = 0; t < 64; ++t) {
            finite &= std::isfinite(r.out[t]);
            worst = std::max(worst, static_cast<double>(std::fabs(r.out[t])));
          }
        }
        INFO(kModeNames[mode] << " prewarp " << prewarp << " stages " << stages << " worst " << worst);
        CHECK(finite);
        CHECK(worst < 1000.0);  // ±18 dB gains, a white input of amplitude 1
      }
}
}  // namespace

TEST_CASE("svf: audio-rate cutoff / q / gain sweeps stay bounded and finite (every mode, both prewarps, 1 / 8 stages)") {
  audioRateSweep({1u, 8u});
}

TEST_CASE("svf: audio-rate sweeps stay bounded and finite at 17 and 64 stages (the double cascade)" * doctest::test_suite("slow")) {
  audioRateSweep({17u, 64u});
}

TEST_CASE("svf: any split of the frames into calls gives the same bits (param and audio rate, 1 and 8 lanes)") {
  pq::DenormalGuard dg;
  for (uint32_t lanes : {1u, 8u})
    for (bool audio : {false, true}) {
      INFO("lanes " << lanes << (audio ? " audio rate" : " param rate"));
      constexpr uint32_t kN = 2000;
      std::mt19937 rng(3);
      std::uniform_real_distribution<float> u(-1.0f, 1.0f);
      std::vector<float> x(kN * lanes), cut(kN * lanes);
      for (uint32_t i = 0; i < kN * lanes; ++i) {
        x[i] = u(rng);
        cut[i] = 500.0f + 400.0f * std::sin(i * 0.003f);
      }
      std::vector<float> got[2];
      for (int run = 0; run < 2; ++run) {
        Rig r(lanes, 512);
        Filt f = filt(5, 4, 0, 800.0f);  // a 4-stage matched low shelf
        r.setPatterns({f});
        std::mt19937 split(run == 0 ? 1 : 2);
        for (uint32_t t0 = 0; t0 < kN;) {
          const uint32_t n = run == 0 ? std::min<uint32_t>(64, kN - t0) : std::min<uint32_t>(1 + split() % 97, kN - t0);
          std::copy_n(x.begin() + t0 * lanes, n * lanes, r.in[0].data());
          if (audio) std::copy_n(cut.begin() + t0 * lanes, n * lanes, r.in[1].data());
          r.call(n, audio ? 0x7Cu : kAllSpl);  // param rate: the pattern setPatterns wrote (800 Hz)
          got[run].insert(got[run].end(), r.out.data(), r.out.data() + n * lanes);
          t0 += n;
        }
      }
      CHECK(std::memcmp(got[0].data(), got[1].data(), got[0].size() * 4) == 0);
    }
}

TEST_CASE("svf: a NaN input sample does not poison the filter") {
  pq::DenormalGuard dg;
  Rig r(1, 64);
  r.setPatterns({filt(4, 1, 0, 1000.0f)});
  for (int blk = 0; blk < 4; ++blk) {
    for (uint32_t t = 0; t < 64; ++t) r.in[0][t] = blk == 1 && t == 10 ? std::nanf("") : 0.1f * std::sin(t * 0.3f);
    r.call(64, kAllSpl);
  }
  bool finite = true;
  for (uint32_t t = 0; t < 64; ++t) finite &= std::isfinite(r.out[t]);
  CHECK(finite);  // the block after the NaN's call is clean again
}

TEST_CASE("svf: no allocation on the process path (designs at param and audio rate, 1 and 8 lanes)") {
  pq::DenormalGuard dg;
  uint64_t violations = 0;
  for (uint32_t lanes : {1u, 8u})
    for (uint32_t mode = 0; mode < kSvfModeCount; ++mode) {
      Rig r(lanes, 64);
      r.setPatterns({filt(mode, 8, mode & 1, 1000.0f)});
      for (uint32_t i = 0; i < 64 * 8; ++i) {
        r.in[0][i] = std::sin(i * 0.1f);
        r.in[1][i] = 200.0f + i;  // audio-rate cutoff
      }
      violations += pq::alloc_trap::countRtViolations([&] {
        for (int blk = 0; blk < 4; ++blk) {
          for (uint32_t l = 0; l < 8; ++l) r.in[2][l] = 0.5f + 0.25f * blk;  // a param change: a design per block
          r.call(64, kAllSpl);
          r.call(64, 0x7Cu);  // the cutoff at audio rate: a design per frame
        }
      });
    }
  CHECK(violations == 0);
}


// ---- EQ.1b: state size, param ramps, exactness at rest ---------------------------------------------------------------
namespace {

// A ramp of cutoff, q and gain over 20 ms (960 frames at 48 kHz), with the param ramp kernel's arithmetic
// (target − inc·rem: within a few ulp of its line), held before and after.
struct Ramp {
  const char* name;
  float from[3], to[3];
};
constexpr Ramp kRamps[] = {
    {"fc 100 -> 10k", {100, 0.7071f, 4}, {10000, 0.7071f, 4}},
    {"fc 10k -> 100", {10000, 0.7071f, 4}, {100, 0.7071f, 4}},
    {"fc 20 -> 20k", {20, 0.7071f, 4}, {20000, 0.7071f, 4}},
    {"fc 20k -> 20", {20000, 0.7071f, 4}, {20, 0.7071f, 4}},
    {"fc 1k -> 1.2k", {1000, 0.7071f, 4}, {1200, 0.7071f, 4}},
    {"q 0.5 -> 10 at 1k", {1000, 0.5f, 4}, {1000, 10, 4}},
    {"gain -18 -> +18 dB at 1k", {1000, 0.7071f, 0.1259f}, {1000, 0.7071f, 7.943f}},
    {"gain +18 -> -18 dB at 200", {200, 2.0f, 7.943f}, {200, 2.0f, 0.1259f}},
    {"all three", {300, 0.5f, 0.5f}, {5000, 3.0f, 3.0f}},
};
constexpr uint32_t kPre = 128, kRampLen = 960, kPost = 512, kRun = kPre + kRampLen + kPost;

float rampAt(const Ramp& r, uint32_t i, uint32_t t) {
  if (t < kPre) return r.from[i];
  const float inc = (r.to[i] - r.from[i]) / static_cast<float>(kRampLen);
  const float rem = static_cast<float>(kPre + kRampLen - 1) - static_cast<float>(t);
  return rem > 0.0f ? r.to[i] - inc * rem : r.to[i];
}

// The kernel on 1 or 8 lanes (lane l: ramp ramps[l]), cutoff / q / gain Blk, in calls of 64 frames; per lane outputs.
std::vector<std::vector<float>> runRamps(const Filt& f, const std::vector<const Ramp*>& ramps, const std::vector<float>& x) {
  const uint32_t lanes = static_cast<uint32_t>(ramps.size());
  Rig r(lanes, 64);
  r.setPatterns({f});
  std::vector<std::vector<float>> out(lanes);
  for (uint32_t t0 = 0; t0 < kRun; t0 += 64) {
    const uint32_t n = std::min<uint32_t>(64, kRun - t0);
    for (uint32_t t = 0; t < n; ++t)
      for (uint32_t l = 0; l < lanes; ++l) {
        r.in[0][t * lanes + l] = x[t0 + t];
        for (uint32_t i = 0; i < 3; ++i) r.in[1 + i][t * lanes + l] = rampAt(*ramps[l], i, t0 + t);
      }
    r.call(n, 0x70u);
    for (uint32_t t = 0; t < n; ++t)
      for (uint32_t l = 0; l < lanes; ++l) out[l].push_back(r.out[t * lanes + l]);
  }
  return out;
}

// The reference: a design per frame (svfDesign of that frame's inputs, the kernel's float coefficients), run in double.
std::vector<double> perFrameDesign(const Filt& f, const Ramp& ramp, const std::vector<float>& x) {
  std::vector<double> y;
  std::vector<double> ic(2 * kSvfMaxStages, 0.0);
  std::vector<SvfSection> s(kSvfMaxStages);
  for (uint32_t t = 0; t < kRun; ++t) {
    Filt g = f;
    g.cutoff = rampAt(ramp, 0, t);
    g.q = rampAt(ramp, 1, t);
    g.gain = rampAt(ramp, 2, t);
    const uint32_t n = svfDesign(g.params(), s.data());
    double v = x[t];
    for (uint32_t k = 0; k < n; ++k) {
      const SvfKernelCoefs c = svfKernelCoefs(s[k]);
      double& i1 = ic[2 * k];
      double& i2 = ic[2 * k + 1];
      const double w = v - i2;
      const double n1 = c.b1 * w + (c.d1 * i1 + i1), n2 = c.b2 * w + (c.b1 * i1 + i2);
      v = c.h1 * (i1 + n1) + (c.h2 * (i2 + n2) + c.m0 * v);
      i1 = n1;
      i2 = n2;
    }
    y.push_back(v);
  }
  return y;
}

}  // namespace

TEST_CASE("svf (EQ.1b): 20 ms param ramps follow a design per frame within -90 dB, no clicks (every mode, 1 / 8 "
          "stages, both prewarps); 8 lanes = 1 lane bit for bit") {
  pq::DenormalGuard dg;
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> x(kRun);
  for (float& v : x) v = u(rng);
  double worstAll = -300.0;
  std::string worstWhere;
  for (uint32_t mode = 0; mode < kSvfModeCount; ++mode)
    for (uint32_t stages : {1u, 8u})
      for (uint32_t prewarp : {0u, 1u}) {
        const auto m = static_cast<SvfMode>(mode);
        if ((m == SvfMode::Bell || m == SvfMode::Notch) && stages > 1) continue;
        if (prewarp == 1 && stages > 1) continue;
        Filt f;
        f.mode = mode;
        f.stages = stages;
        f.prewarp = prewarp;
        std::vector<const Ramp*> all;
        for (const Ramp& r : kRamps) all.push_back(&r);
        const std::vector<const Ramp*> eight(all.begin(), all.begin() + 8);
        const auto got8 = runRamps(f, eight, x);
        for (size_t k = 0; k < all.size(); ++k) {
          const Ramp& r = *all[k];
          INFO(std::string(kModeNames[mode]) << " stages " << stages << (prewarp ? " bilinear " : " matched ") << r.name);
          const std::vector<float> got = runRamps(f, {&r}, x)[0];
          if (k < 8) CHECK(std::memcmp(got.data(), got8[k].data(), got.size() * 4) == 0);
          const std::vector<double> want = perFrameDesign(f, r, x);
          double e2 = 0.0, r2 = 0.0, emax = 0.0, rmax = 0.0;
          for (uint32_t t = 0; t < kRun; ++t) {
            const double d = got[t] - want[t];
            e2 += d * d;
            r2 += want[t] * want[t];
            emax = std::max(emax, std::fabs(d));
            rmax = std::max(rmax, std::fabs(want[t]));
          }
          const double db = 10.0 * std::log10(e2 / r2 + 1e-300);
          if (db > worstAll) {
            worstAll = db;
            worstWhere = std::string(kModeNames[mode]) + " s" + std::to_string(stages) + (prewarp ? " bil " : " mat ") + r.name;
          }
          CHECK(db <= -90.0);
          CHECK(emax <= 1e-4 * rmax);  // no click: the largest deviation is far below the signal
        }
      }
  MESSAGE("ramps vs a design per frame: worst " << worstAll << " dB (" << worstWhere << ")");
}

TEST_CASE("svf (EQ.1b): eight lanes on the same ramp are each the 1-lane filter (one design for the group)") {
  pq::DenormalGuard dg;
  std::mt19937 rng(12);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> x(kRun);
  for (float& v : x) v = u(rng);
  for (uint32_t mode : {0u, 5u}) {
    Filt f;
    f.mode = mode;
    f.stages = 4;
    for (size_t k : {size_t{4}, size_t{8}}) {
      const Ramp& r = kRamps[k];
      INFO(std::string(kModeNames[mode]) << " " << r.name);
      const std::vector<float> one = runRamps(f, {&r}, x)[0];
      const auto eight = runRamps(f, std::vector<const Ramp*>(8, &r), x);
      for (uint32_t l = 0; l < 8; ++l) CHECK(std::memcmp(one.data(), eight[l].data(), one.size() * 4) == 0);
    }
  }
}

TEST_CASE("svf (EQ.1b): after a ramp (and with gain ramped on a mode without a gain) the filter runs its exact design") {
  pq::DenormalGuard dg;
  for (uint32_t lanes : {1u, 8u})
    for (uint32_t mode : {0u, 4u, 5u, 6u}) {
      INFO("lanes " << lanes << " " << std::string(kModeNames[mode]));
      const Filt target = filt(mode, mode == 4 ? 1 : 6, 0, 2400.0f);
      // A: ramps from 300 Hz (and half the gain) to the target, its inputs Blk throughout (held after the ramp);
      // B: the target as a pattern from the start.
      Rig a(lanes, 64), b(lanes, 64);
      a.setPatterns({target});
      b.setPatterns({target});
      std::mt19937 rng(13);
      std::uniform_real_distribution<float> u(-1.0f, 1.0f);
      auto block = [&](uint32_t t0, bool noise) {
        for (uint32_t t = 0; t < 64; ++t) {
          const float frac = std::min(1.0f, static_cast<float>(t0 + t) / 960.0f);
          for (uint32_t l = 0; l < lanes; ++l) {
            const float xv = noise ? u(rng) : 0.0f;
            a.in[0][t * lanes + l] = xv;
            b.in[0][t * lanes + l] = xv;
            a.in[1][t * lanes + l] = 300.0f + (target.cutoff - 300.0f) * frac;
            a.in[2][t * lanes + l] = target.q;
            // a lowpass ignores its gain: ramped all the time here, it must not move the filter
            a.in[3][t * lanes + l] = mode == 0 ? 0.5f + 0.001f * static_cast<float>(t0 + t)
                                               : target.gain * (0.5f + 0.5f * frac);
          }
        }
        a.call(64, 0x70u);
        b.call(64, kAllSpl);
      };
      uint32_t t0 = 0;
      for (; t0 < 1600; t0 += 64) block(t0, true);
      // Silence until both have decayed to exactly 0 (FTZ), then the same noise: the same bits iff the same coefficients.
      for (int k = 0; k < 2000; ++k, t0 += 64) {
        block(t0, false);
        if (decayStateSmall(Atom::Svf, a.state.data(), 0.0f) && decayStateSmall(Atom::Svf, b.state.data(), 0.0f)) break;
      }
      REQUIRE(decayStateSmall(Atom::Svf, a.state.data(), 0.0f));
      REQUIRE(decayStateSmall(Atom::Svf, b.state.data(), 0.0f));
      bool same = true;
      for (int k = 0; k < 8; ++k, t0 += 64) {
        block(t0, true);
        same = same && std::memcmp(a.out.data(), b.out.data(), static_cast<size_t>(64) * lanes * 4) == 0;
      }
      CHECK(same);
    }
}

TEST_CASE("svf (EQ.1b): the state is sized by its sections; a state holds what it was sized for and no more") {
  StateInit init;
  init.svfSections = 1;
  init.lanes = 1;
  CHECK(stateBytes(Atom::Svf, init) <= 1024);
  CHECK(stateBytes(Atom::Svf, init) == kSvfHeaderBytes + 96);  // 22 fields of 4 bytes, rounded to 32
  init.lanes = 8;
  CHECK(stateBytes(Atom::Svf, init) == kSvfHeaderBytes + kSvfFields * 32);
  init.svfSections = 64;
  CHECK(stateBytes(Atom::Svf, init) == sizeof(SvfState));
  init.svfSections = 0;  // clamped to 1 … 64
  CHECK(stateBytes(Atom::Svf, init) == kSvfHeaderBytes + kSvfFields * 32);
  // A 2-section state: a 2-stage lowpass in it is its design; a 64-stage const runs its first 2 sections and never
  // writes past the state (param and audio rate).
  pq::DenormalGuard dg;
  for (uint32_t lanes : {1u, 8u}) {
    INFO("lanes " << lanes);
    StateInit si;
    si.lanes = lanes;
    si.svfSections = 2;
    const size_t bytes = stateBytes(Atom::Svf, si);
    pq::AlignedBuffer<uint8_t> mem(bytes + 256);
    std::memset(mem.data() + bytes, 0xA5, 256);
    initState(Atom::Svf, mem.data(), si);
    Rig r(lanes, 64);
    for (uint32_t stages : {2u, 64u}) {
      r.setPatterns({filt(0, stages, 0, 1000.0f)});
      for (uint32_t i = 0; i < 64 * 8; ++i) {
        r.in[0][i] = std::sin(0.05f * static_cast<float>(i));
        r.in[1][i] = 900.0f + static_cast<float>(i);  // audio rate: the segment fields too
      }
      if (stages == 2) {  // the impulse response of the sized state = svfResponse
        Rig full(lanes, 64);  // the full-size state, the same inputs
        for (uint32_t k = 0; k < 7; ++k)
          for (uint32_t i = 0; i < 64 * 8; ++i) full.in[k][i] = r.in[k][i];
        full.call(64, kAllSpl);
        const float* ins[7];
        for (uint32_t i = 0; i < 7; ++i) ins[i] = r.in[i].data();
        float* outs[1] = {r.out.data()};
        const KernelArgs ka{ins, outs, mem.data(), nullptr, 64, 7, 1, kAllSpl};
        findKernel(Atom::Svf, lanes, kAllSpl)(ka);
        CHECK(std::memcmp(full.out.data(), r.out.data(), static_cast<size_t>(64) * lanes * 4) == 0);
      }
      const float* ins[7];
      for (uint32_t i = 0; i < 7; ++i) ins[i] = r.in[i].data();
      float* outs[1] = {r.out.data()};
      for (uint32_t spl : {kAllSpl, 0x7Cu}) {
        const KernelArgs ka{ins, outs, mem.data(), nullptr, 64, 7, 1, spl};
        findKernel(Atom::Svf, lanes, spl)(ka);
      }
      bool finite = true;
      for (uint32_t i = 0; i < 64 * lanes; ++i) finite = finite && std::isfinite(r.out[i]);
      CHECK(finite);
    }
    bool canary = true;
    for (size_t i = 0; i < 256; ++i) canary = canary && mem[bytes + i] == 0xA5;
    CHECK(canary);
  }
}

TEST_CASE("svf (EQ.1b): no allocation on the ramp, lane-follow and held-block paths (1 and 8 lanes)") {
  pq::DenormalGuard dg;
  uint64_t violations = 0;
  for (uint32_t lanes : {1u, 8u})
    for (uint32_t mode : {0u, 5u}) {
      Rig r(lanes, 64);
      r.setPatterns({filt(mode, 8, 0, 1000.0f)});
      violations += pq::alloc_trap::countRtViolations([&] {
        for (uint32_t t0 = 0; t0 + 64 <= kRun; t0 += 64) {  // a ramp (segments; 8 lanes on one ramp: followed), held
          for (uint32_t t = 0; t < 64; ++t)
            for (uint32_t l = 0; l < lanes; ++l) {
              r.in[0][t * lanes + l] = 0.25f;
              for (uint32_t i = 0; i < 3; ++i) r.in[1 + i][t * lanes + l] = rampAt(kRamps[8], i, t0 + t);
            }
          r.call(64, 0x70u);
        }
      });
    }
  CHECK(violations == 0);
}
