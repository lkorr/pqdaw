// Fast math vs std:: (ns per element). Each kernel maps a 4096-element input buffer (values spread over the
// function's typical domain) to an output buffer, so this is throughput, as in a block-processing loop.
//   std      : std:: float function, scalar loop (MSVC may auto-vectorise these with its own SVML-style library)
//   fast     : pq::fastX(float) scalar loop
//   fast8    : pq::fastX(__m256) loop, 8 per iteration

#include <immintrin.h>

#include <cmath>
#include <cstdio>

#include "pq/core/cpu.h"
#include "pq/core/fast_math.h"
#include "pq/core/units.h"
#include "pq_bench.h"

namespace {

constexpr std::size_t kN = 4096;

struct Buffers {
  alignas(32) float in[kN];
  alignas(32) float out[kN];
};

void fill(float* p, float lo, float hi) {
  // Deterministic, non-monotonic spread (so branchy std:: paths see realistic variety).
  uint32_t s = 12345;
  for (std::size_t i = 0; i < kN; ++i) {
    s = s * 1664525u + 1013904223u;
    p[i] = lo + (hi - lo) * static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
  }
}

template <class Scalar, class Wide, class Std>
void run(const char* name, Buffers& b, float lo, float hi, Std stdf, Scalar fast, Wide wide) {
  fill(b.in, lo, hi);
  char label[96];

  auto sStd = pq::bench::measure([&] {
    for (std::size_t i = 0; i < kN; ++i) b.out[i] = stdf(b.in[i]);
    pq::bench::doNotOptimize(b.out[kN - 1]);
  }, kN);
  std::snprintf(label, sizeof label, "%s/std", name);
  pq::bench::report(label, sStd, "ns/elem");

  auto sFast = pq::bench::measure([&] {
    for (std::size_t i = 0; i < kN; ++i) b.out[i] = fast(b.in[i]);
    pq::bench::doNotOptimize(b.out[kN - 1]);
  }, kN);
  std::snprintf(label, sizeof label, "%s/fast", name);
  pq::bench::report(label, sFast, "ns/elem");

  auto sWide = pq::bench::measure([&] {
    for (std::size_t i = 0; i < kN; i += 8) _mm256_store_ps(b.out + i, wide(_mm256_load_ps(b.in + i)));
    pq::bench::doNotOptimize(b.out[kN - 1]);
  }, kN);
  std::snprintf(label, sizeof label, "%s/fast8", name);
  pq::bench::report(label, sWide, "ns/elem");

  std::printf("      speedup vs std: fast %.1fx, fast8 %.1fx\n", sStd.medianNs / sFast.medianNs,
              sStd.medianNs / sWide.medianNs);
}

}  // namespace

int main() {
  std::printf("CPU: %s\n", pq::cpuFeatures().brand.c_str());
  static Buffers b;

  run("exp2 [-20,20]", b, -20.0f, 20.0f, [](float x) { return std::exp2(x); },
      [](float x) { return pq::fastExp2(x); }, [](__m256 x) { return pq::fastExp2(x); });
  run("log2 [1e-3,1e5]", b, 1e-3f, 1e5f, [](float x) { return std::log2(x); },
      [](float x) { return pq::fastLog2(x); }, [](__m256 x) { return pq::fastLog2(x); });
  run("sin [-10,10]", b, -10.0f, 10.0f, [](float x) { return std::sin(x); },
      [](float x) { return pq::fastSin(x); }, [](__m256 x) { return pq::fastSin(x); });
  run("tan [0,1.57]", b, 0.0f, 1.57f, [](float x) { return std::tan(x); },
      [](float x) { return pq::fastTan(x); }, [](__m256 x) { return pq::fastTan(x); });
  run("tanh [-5,5]", b, -5.0f, 5.0f, [](float x) { return std::tanh(x); },
      [](float x) { return pq::fastTanh(x); }, [](__m256 x) { return pq::fastTanh(x); });

  // Units: the precise reference path is double; the fast path is float.
  run("centsToHz [-6000,6000]", b, -6000.0f, 6000.0f,
      [](float c) { return static_cast<float>(pq::centsToHz(c, 261.626)); },
      [](float c) { return pq::fastCentsToHz(c, 261.626f); },
      [](__m256 c) { return pq::fastCentsToHz(c, _mm256_set1_ps(261.626f)); });
  run("hzToCents [20,20000]", b, 20.0f, 20000.0f,
      [](float hz) { return static_cast<float>(pq::hzToCents(hz, 261.626)); },
      [](float hz) { return pq::fastHzToCents(hz, 261.626f); },
      [](__m256 hz) { return pq::fastHzToCents(hz, _mm256_set1_ps(261.626f)); });
  run("dbToGain [-100,24]", b, -100.0f, 24.0f, [](float db) { return static_cast<float>(pq::dbToGain(db)); },
      [](float db) { return pq::fastDbToGain(db); }, [](__m256 db) { return pq::fastDbToGain(db); });
  run("gainToDb [1e-5,4]", b, 1e-5f, 4.0f, [](float g) { return static_cast<float>(pq::gainToDb(g)); },
      [](float g) { return pq::fastGainToDb(g); }, [](__m256 g) { return pq::fastGainToDb(g); });
  return 0;
}
