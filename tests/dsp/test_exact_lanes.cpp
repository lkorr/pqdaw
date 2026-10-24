// QUESTIONS Q94: the exact transcendentals (sin, tan, tanh, exp, log, pow, cents/Hz and dB/gain conversions) give
// the same bits at every lane count. The 1-lane path (a global op's block remainder, a per-sample loop) calls the
// 8-lane SVML functions on a broadcast (simd.h S1), so it equals every lane of an 8-lane voice group / Control
// pattern / constant fold. Swept over float bit patterns (every 2^k-th, so zeros, denormals, infinities and NaNs
// are in), the 8 lanes of each 8-lane frame holding 8 different arguments.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"

using pq::dsp::Atom;

namespace {

constexpr uint32_t kChunk8 = 64;  // frames per 8-lane call (512 arguments)
constexpr uint32_t kTail = 7;     // frames per 1-lane call: below 8, so every sample takes the S1 path

// Runs `a` on args[k] (one vector per input) through the 8-lane kernel and through 7-frame 1-lane calls; returns
// the number of results whose bits differ (NaNs included: both paths call the same SVML function).
uint64_t compareLanes(Atom a, const std::vector<std::vector<float>>& args, std::string& first) {
  pq::DenormalGuard dg;  // as on the audio thread (and in constant folding)
  const uint32_t nIn = static_cast<uint32_t>(args.size());
  const size_t n = args[0].size() / (kChunk8 * 8) * (kChunk8 * 8);
  const pq::dsp::KernelFn f1 = pq::dsp::findKernel(a, 1, 0);
  const pq::dsp::KernelFn f8 = pq::dsp::findKernel(a, 8, 0);
  REQUIRE(f1 != nullptr);
  REQUIRE(f8 != nullptr);
  std::vector<pq::AlignedBuffer<float>> in8;
  for (uint32_t i = 0; i < nIn; ++i) in8.emplace_back(kChunk8 * 8);
  pq::AlignedBuffer<float> out8(kChunk8 * 8), out1(kChunk8 * 8);
  const float* in8p[3]{};
  float* out8p[1] = {out8.data()};
  uint64_t diff = 0;
  for (size_t base = 0; base < n; base += kChunk8 * 8) {
    for (uint32_t i = 0; i < nIn; ++i) {
      std::memcpy(in8[i].data(), args[i].data() + base, kChunk8 * 8 * sizeof(float));
      in8p[i] = in8[i].data();
    }
    f8(pq::dsp::KernelArgs{in8p, out8p, nullptr, nullptr, kChunk8, static_cast<uint16_t>(nIn), 1, 0});
    for (uint32_t off = 0; off < kChunk8 * 8; off += kTail) {
      const uint32_t frames = std::min(kTail, kChunk8 * 8 - off);
      const float* in1p[3]{};
      for (uint32_t i = 0; i < nIn; ++i) in1p[i] = args[i].data() + base + off;
      float* out1p[1] = {out1.data() + off};
      f1(pq::dsp::KernelArgs{in1p, out1p, nullptr, nullptr, frames, static_cast<uint16_t>(nIn), 1, 0});
    }
    for (uint32_t k = 0; k < kChunk8 * 8; ++k) {
      if (std::bit_cast<uint32_t>(out1[k]) == std::bit_cast<uint32_t>(out8[k])) continue;
      if (diff++ == 0) {
        auto hex = [](float v) {
          char buf[48];
          std::snprintf(buf, sizeof buf, "%a", static_cast<double>(v));
          return std::string(buf);
        };
        first = std::string(pq::dsp::kernelInfo(a).name) + "(";
        for (uint32_t i = 0; i < nIn; ++i) first += (i ? ", " : "") + hex(args[i][base + k]);
        first += "): 1 lane " + hex(out1[k]) + ", 8 lanes " + hex(out8[k]);
      }
    }
  }
  return diff;
}

// Every `stride`-th float bit pattern (stride a power of two), offset so the sweep is not all multiples of it.
std::vector<float> bitSweep(uint32_t stride, uint32_t offset) {
  std::vector<float> v;
  v.reserve(static_cast<size_t>((1ull << 32) / stride));
  for (uint64_t b = offset % stride; b < (1ull << 32); b += stride) v.push_back(std::bit_cast<float>(static_cast<uint32_t>(b)));
  while (v.size() % (kChunk8 * 8) != 0) v.push_back(0.0f);
  return v;
}

// Realistic second operands (exponents, reference pitches) and the special ones.
std::vector<float> secondOperands(Atom a) {
  if (a == Atom::Pow)
    return {-3.5f, -2.0f, -1.0f, -0.5f, -0.3894f, 0.0f, -0.0f, 0.2823f, 0.5f, 0.8515f, 1.0f, 1.7372f, 2.0f, 3.0f,
            7.25f, 1e-8f, 40.0f, -40.0f, INFINITY, -INFINITY, NAN, 1e30f};
  return {261.6256f, 440.0f, 432.0f, 1.0f, 27.5f, 4186.01f, 1e-30f, 0.0f, -440.0f, INFINITY, NAN, 1e30f};
}

struct SweepResult {
  uint64_t values = 0, differ = 0;
};

SweepResult sweep(Atom a, uint32_t stride) {
  SweepResult r;
  std::string first;
  if (pq::dsp::kernelInfo(a).numIn == 1) {
    const std::vector<float> x = bitSweep(stride, 0x9E37u);
    r.values = x.size();
    r.differ = compareLanes(a, {x}, first);
  } else {
    // Binary: the first operand over the bit sweep (64× sparser), the second over a fixed set; plus the swapped
    // roles for pow (its exponent over the sweep).
    const std::vector<float> x = bitSweep(stride * 64, 0x51EDu);
    for (float b : secondOperands(a)) {
      std::vector<float> y(x.size(), b);
      r.values += x.size();
      r.differ += compareLanes(a, {x, y}, first);
      if (a == Atom::Pow) {
        r.values += x.size();
        r.differ += compareLanes(a, {y, x}, first);
      }
    }
  }
  CHECK_MESSAGE(r.differ == 0, r.differ << " of " << r.values << " differ; first: " << first);
  return r;
}

constexpr Atom kExact[] = {Atom::Sin, Atom::Tan,      Atom::Tanh,      Atom::Exp,      Atom::Log,
                           Atom::Pow, Atom::CentsToHz, Atom::HzToCents, Atom::DbToGain, Atom::GainToDb};

}  // namespace

TEST_CASE("dsp exact family: the 1-lane kernels equal the 8-lane kernels bit for bit (every 2^16-th float)") {
  uint64_t values = 0;
  for (Atom a : kExact) {
    CAPTURE(pq::dsp::kernelInfo(a).name);
    values += sweep(a, 1u << 16).values;
  }
  MESSAGE("compared " << values << " results");
}

TEST_CASE("dsp exact family: the 1-lane kernels equal the 8-lane kernels bit for bit (every 2^9-th float)" * doctest::test_suite("slow")) {
  uint64_t values = 0;
  for (Atom a : kExact) {
    CAPTURE(pq::dsp::kernelInfo(a).name);
    values += sweep(a, 1u << 9).values;
  }
  MESSAGE("compared " << values << " results");
}
