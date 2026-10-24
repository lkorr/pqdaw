#pragma once

// Test harness for leaf-atom kernels: drives a kernel block by block (random block sizes, both lane counts,
// every Blk/Spl input combination) and compares every lane with a naive double-precision reference.

#include <doctest/doctest.h>

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"

namespace pqtest {

using pq::dsp::Atom;

// One input series for one lane, T samples. Bool inputs are canonical masks.
using Gen = std::function<std::vector<float>(std::mt19937& rng, uint32_t lane, uint32_t T)>;

// Reference for ONE lane over the whole timeline: in[i][t] (bool inputs as 0/1) → out[o][t] (bool as 0/1).
using RefFn = std::function<void(const std::vector<std::vector<double>>& in, std::vector<std::vector<double>>& out)>;

struct Tol {
  double abs = 0;
  double rel = 0;
  double period = 0;     // > 0: compare modulo this period (phases)
  int periodInput = -1;  // ≥ 0: compare modulo |in[periodInput][t]| (mod)
};

struct AtomSpec {
  Atom atom{};
  std::vector<Gen> gens;
  RefFn ref;
  std::vector<Tol> tol;  // per output (missing entries: exact)
  pq::dsp::StateInit init{};
  const void* resource = nullptr;
  uint32_t numOut = 0;  // 0: from KernelInfo
  uint32_t T = 700;
  uint32_t maxBlock = 67;
  std::vector<uint32_t> lanesList{1, 8};
};

inline float maskF(bool b) { return pq::dsp::boolMask(b); }

// ---- common generators --------------------------------------------------------------------------------------
inline Gen uniform(float lo, float hi) {
  return [lo, hi](std::mt19937& rng, uint32_t, uint32_t T) {
    std::uniform_real_distribution<float> d(lo, hi);
    std::vector<float> v(T);
    for (auto& x : v) x = d(rng);
    return v;
  };
}
// Values from a small grid so that ties / equalities happen often.
inline Gen grid(float lo, float step, int count) {
  return [=](std::mt19937& rng, uint32_t, uint32_t T) {
    std::uniform_int_distribution<int> d(0, count - 1);
    std::vector<float> v(T);
    for (auto& x : v) x = lo + step * static_cast<float>(d(rng));
    return v;
  };
}
inline Gen pickFrom(std::vector<float> values) {
  return [values](std::mt19937& rng, uint32_t, uint32_t T) {
    std::uniform_int_distribution<size_t> d(0, values.size() - 1);
    std::vector<float> v(T);
    for (auto& x : v) x = values[d(rng)];
    return v;
  };
}
// Bool series that toggles with probability p per sample (so edges are frequent but not every sample).
inline Gen boolRuns(double p) {
  return [p](std::mt19937& rng, uint32_t, uint32_t T) {
    std::bernoulli_distribution d(p);
    std::vector<float> v(T);
    bool b = std::bernoulli_distribution(0.5)(rng);
    for (auto& x : v) {
      if (d(rng)) b = !b;
      x = maskF(b);
    }
    return v;
  };
}
inline Gen constant(float c) {
  return [c](std::mt19937&, uint32_t, uint32_t T) { return std::vector<float>(T, c); };
}
// Positive or negative magnitudes in [lo, hi].
inline Gen signedMagnitude(float lo, float hi) {
  return [lo, hi](std::mt19937& rng, uint32_t, uint32_t T) {
    std::uniform_real_distribution<float> d(lo, hi);
    std::bernoulli_distribution s(0.5);
    std::vector<float> v(T);
    for (auto& x : v) x = s(rng) ? d(rng) : -d(rng);
    return v;
  };
}

inline bool isMaskCanonical(float f) {
  const uint32_t u = std::bit_cast<uint32_t>(f);
  return u == 0u || u == 0xFFFFFFFFu;
}

// Results of one kernel run, [output][t*lanes + lane].
struct RunResult {
  std::vector<std::vector<float>> out;
  std::vector<std::vector<float>> in;  // inputs after Spl flattening, [input][t*lanes + lane]
};

// Runs the kernel for `lanes` and Spl mask over the inputs (layout [t*lanes + lane]) with the given block
// partition. Spl inputs are flattened in `in` to their block-start value first (that is what a Spl input
// means), so the reference sees exactly what the kernel saw.
inline RunResult runKernel(const AtomSpec& s, uint32_t lanes, uint32_t splMask, std::vector<std::vector<float>> in,
                           const std::vector<uint32_t>& blocks, uint32_t numOut) {
  const auto& info = pq::dsp::kernelInfo(s.atom);
  const uint32_t nIn = static_cast<uint32_t>(in.size());
  RunResult r;
  const uint32_t T = static_cast<uint32_t>(in.empty() ? s.T : in[0].size() / lanes);
  // Flatten Spl inputs per block.
  {
    uint32_t t0 = 0;
    for (uint32_t b : blocks) {
      for (uint32_t i = 0; i < nIn; ++i) {
        if (!((splMask >> i) & 1u) || b == 0) continue;
        for (uint32_t t = t0 + 1; t < t0 + b; ++t)
          for (uint32_t l = 0; l < lanes; ++l) in[i][t * lanes + l] = in[i][t0 * lanes + l];
      }
      t0 += b;
    }
  }
  pq::dsp::StateInit init = s.init;
  init.lanes = lanes;
  // The state block, then 8 canary floats: a kernel must never write outside its stateBytes().
  const size_t stateFloats = (pq::dsp::stateBytes(s.atom, init) + 3) / 4;
  constexpr uint32_t kStateCanary = 8;
  constexpr float kStateCanaryValue = -9876.5f;
  pq::AlignedBuffer<float> state(stateFloats + kStateCanary);
  for (uint32_t k = 0; k < kStateCanary; ++k) state[stateFloats + k] = kStateCanaryValue;
  pq::dsp::initState(s.atom, state.data(), init);
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(s.atom, lanes, splMask);
  REQUIRE(fn != nullptr);

  r.out.assign(numOut, std::vector<float>(static_cast<size_t>(T) * lanes, 0.0f));
  constexpr uint32_t kCanary = 16;
  constexpr float kCanaryValue = 1234.5f;
  std::vector<pq::AlignedBuffer<float>> inBufs;
  std::vector<pq::AlignedBuffer<float>> outBufs;
  for (uint32_t i = 0; i < nIn; ++i) inBufs.emplace_back(static_cast<size_t>(s.maxBlock) * lanes + 8);
  for (uint32_t o = 0; o < numOut; ++o) outBufs.emplace_back(static_cast<size_t>(s.maxBlock) * lanes + kCanary);
  std::vector<const float*> inPtrs(nIn);
  std::vector<float*> outPtrs(numOut);
  uint32_t t0 = 0;
  for (uint32_t b : blocks) {
    REQUIRE(b <= s.maxBlock);
    for (uint32_t i = 0; i < nIn; ++i) {
      float* buf = inBufs[i].data();
      if ((splMask >> i) & 1u) {
        for (uint32_t k = 0; k < 8; ++k) {
          const uint32_t l = lanes == 8 ? k : 0;
          buf[k] = in[i][static_cast<size_t>(t0 < T ? t0 : 0) * lanes + l];
        }
      } else {
        std::memcpy(buf, in[i].data() + static_cast<size_t>(t0) * lanes, static_cast<size_t>(b) * lanes * 4);
      }
      inPtrs[i] = buf;
    }
    for (uint32_t o = 0; o < numOut; ++o) {
      float* buf = outBufs[o].data();
      for (size_t k = 0; k < outBufs[o].size(); ++k) buf[k] = kCanaryValue;
      outPtrs[o] = buf;
    }
    pq::dsp::KernelArgs args;
    args.in = inPtrs.data();
    args.out = outPtrs.data();
    args.state = state.data();
    args.resource = s.resource;
    args.frames = b;
    args.numIn = static_cast<uint16_t>(nIn);
    args.numOut = static_cast<uint16_t>(numOut);
    args.splMask = splMask;
    fn(args);
    bool stateOk = true;
    for (uint32_t k = 0; k < kStateCanary; ++k) stateOk &= state[stateFloats + k] == kStateCanaryValue;
    REQUIRE_MESSAGE(stateOk, "kernel wrote past its state block");
    for (uint32_t o = 0; o < numOut; ++o) {
      const float* buf = outBufs[o].data();
      bool canaryOk = true;
      for (size_t k = static_cast<size_t>(b) * lanes; k < outBufs[o].size(); ++k) canaryOk &= buf[k] == kCanaryValue;
      REQUIRE_MESSAGE(canaryOk, "kernel wrote past its output block");
      std::memcpy(r.out[o].data() + static_cast<size_t>(t0) * lanes, buf, static_cast<size_t>(b) * lanes * 4);
    }
    t0 += b;
  }
  (void)info;
  r.in = std::move(in);
  return r;
}

inline std::vector<uint32_t> randomBlocks(std::mt19937& rng, uint32_t T, uint32_t maxBlock) {
  std::vector<uint32_t> blocks;
  std::uniform_int_distribution<uint32_t> d(0, maxBlock);
  uint32_t left = T;
  while (left > 0) {
    uint32_t b = d(rng);
    if (b > left) b = left;
    blocks.push_back(b);
    left -= b;
  }
  return blocks;
}

inline bool closeEnough(double ref, float got, const Tol& tol, double periodHere) {
  if (std::isnan(ref)) return std::isnan(got);
  if (std::isinf(ref)) return static_cast<double>(got) == ref;
  double diff = std::abs(static_cast<double>(got) - ref);
  if (periodHere > 0) diff = std::min(diff, std::abs(diff - periodHere));
  return diff <= tol.abs + tol.rel * std::abs(ref);
}

// Runs the full check: every lane count, every Spl combination of the specialised inputs (required ones always
// Spl), a few random block partitions. Returns the largest observed error over all comparisons (for messages).
inline double checkAtom(const AtomSpec& s, uint32_t seed = 1) {
  const auto& info = pq::dsp::kernelInfo(s.atom);
  const uint32_t nIn = static_cast<uint32_t>(s.gens.size());
  const uint32_t numOut = s.numOut ? s.numOut : info.numOut;
  const uint32_t specMask = info.variadicIn ? ((1u << nIn) - 1u) : info.splSpecMask;
  double worst = 0;
  std::mt19937 rng(seed);
  for (uint32_t lanes : s.lanesList) {
    for (uint32_t m = 0; m <= specMask; ++m) {
      if ((m & ~specMask) != 0) continue;
      const uint32_t splMask = m | info.splRequired;
      for (int partition = 0; partition < 2; ++partition) {
        // Inputs, layout [t*lanes + lane].
        std::vector<std::vector<float>> in(nIn, std::vector<float>(static_cast<size_t>(s.T) * lanes));
        for (uint32_t i = 0; i < nIn; ++i)
          for (uint32_t l = 0; l < lanes; ++l) {
            const auto series = s.gens[i](rng, l, s.T);
            for (uint32_t t = 0; t < s.T; ++t) in[i][static_cast<size_t>(t) * lanes + l] = series[t];
          }
        const auto blocks = randomBlocks(rng, s.T, s.maxBlock);
        const RunResult r = runKernel(s, lanes, splMask, std::move(in), blocks, numOut);
        for (uint32_t l = 0; l < lanes; ++l) {
          std::vector<std::vector<double>> rin(nIn, std::vector<double>(s.T));
          for (uint32_t i = 0; i < nIn; ++i)
            for (uint32_t t = 0; t < s.T; ++t) {
              const float v = r.in[i][static_cast<size_t>(t) * lanes + l];
              rin[i][t] = ((info.boolIn >> i) & 1u) ? (pq::dsp::maskIsTrue(v) ? 1.0 : 0.0) : static_cast<double>(v);
            }
          std::vector<std::vector<double>> rout(numOut, std::vector<double>(s.T, 0.0));
          s.ref(rin, rout);
          for (uint32_t o = 0; o < numOut; ++o) {
            const bool isBool = ((info.boolOut >> o) & 1u) != 0;
            const Tol tol = o < s.tol.size() ? s.tol[o] : Tol{};
            for (uint32_t t = 0; t < s.T; ++t) {
              const float got = r.out[o][static_cast<size_t>(t) * lanes + l];
              const double ref = rout[o][t];
              bool ok;
              if (isBool) {
                ok = isMaskCanonical(got) && (pq::dsp::maskIsTrue(got) == (ref != 0.0));
              } else {
                const double period = tol.periodInput >= 0 ? std::abs(rin[static_cast<size_t>(tol.periodInput)][t])
                                                           : tol.period;
                ok = closeEnough(ref, got, tol, period);
                if (!std::isnan(ref) && !std::isinf(ref)) {
                  double diff = std::abs(static_cast<double>(got) - ref);
                  if (period > 0) diff = std::min(diff, std::abs(diff - period));
                  if (diff > worst) worst = diff;
                }
              }
              if (!ok) {
                std::string inputs;
                for (uint32_t i = 0; i < nIn; ++i) inputs += " in" + std::to_string(i) + "=" + std::to_string(rin[i][t]);
                FAIL_CHECK(std::string(info.name) << " lanes=" << lanes << " spl=" << splMask << " lane=" << l << " t=" << t
                                     << " out" << o << ": got " << got << " ref " << ref << inputs);
                return worst;
              }
            }
          }
        }
      }
    }
  }
  return worst;
}

}  // namespace pqtest
