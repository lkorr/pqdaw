// SPEC §3.1: oscillator phase is a uint32 fixed-point accumulator. This test runs the real phasor kernel for
// 10^9 samples and shows zero drift: the phase after n samples is exactly (inc·n) mod 2^32, where inc is the
// nearest integer to hz·2^32/sr computed here with exact integer arithmetic (independent of the kernel's
// double-precision multiply). So the frequency is exact to the uint32 resolution (sr/2^32 ≈ 1.1e-5 Hz at 48 kHz).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"

namespace {

// Nearest integer (ties to even) to hz·2^32/sr, exactly, for |hz| < 2^24 and integer sr.
int64_t exactIncrement(float hz, uint32_t sr) {
  const bool neg = hz < 0;
  int exp = 0;
  const double mant = std::frexp(std::abs(static_cast<double>(hz)), &exp);  // |hz| = mant·2^exp, mant in [0.5, 1)
  const uint64_t m = static_cast<uint64_t>(std::ldexp(mant, 24));            // exact: float has 24 bits
  const int shift = exp - 24 + 32;                                           // |hz|·2^32 = m·2^shift
  if (shift < 0 || shift > 39) {  // m·2^shift must stay below 2^63
    FAIL("frequency outside the exact-increment helper's range");
  }
  const uint64_t num = m << shift;
  uint64_t q = num / sr;
  const uint64_t r = num % sr;
  if (2 * r > sr || (2 * r == sr && (q & 1))) ++q;
  return neg ? -static_cast<int64_t>(q) : static_cast<int64_t>(q);
}

float unitPhase(uint32_t ph) { return static_cast<float>(ph >> 8) * 5.9604644775390625e-08f; }

}  // namespace

TEST_CASE("phasor: zero drift over 10^9 samples, frequency exact to uint32 resolution" * doctest::test_suite("slow")) {
  constexpr uint32_t kSr = 48000;
  constexpr uint64_t kTotal = 1000000000ull;
  constexpr uint32_t kBlock = 4096;
  const float hz8[8] = {20.0f, 261.626f, 440.0f, 1000.5f, 3.3f, 12345.6f, -110.0f, 0.1f};

  for (uint32_t lanes : {8u, 1u}) {
    pq::dsp::StateInit init;
    init.sampleRate = kSr;
    init.lanes = lanes;
    pq::dsp::PhasorState st{};
    REQUIRE(pq::dsp::stateBytes(pq::dsp::Atom::Phasor, init) == sizeof st);
    pq::dsp::initState(pq::dsp::Atom::Phasor, &st, init);
    // hz is Spl (one value per voice), reset is a Spl "false" pattern: the common unmodulated case.
    alignas(32) float hzPat[8];
    alignas(32) float resetPat[8] = {};
    for (uint32_t k = 0; k < 8; ++k) hzPat[k] = lanes == 8 ? hz8[k] : hz8[1];
    pq::AlignedBuffer<float> out(static_cast<size_t>(kBlock) * lanes);
    const float* ins[] = {hzPat, resetPat};
    float* outs[] = {out.data()};
    const pq::dsp::KernelFn fn = pq::dsp::findKernel(pq::dsp::Atom::Phasor, lanes, 0b11);
    REQUIRE(fn != nullptr);

    uint32_t inc[8];
    for (uint32_t l = 0; l < lanes; ++l) {
      const int64_t e = exactIncrement(hzPat[l], kSr);
      inc[l] = static_cast<uint32_t>(e);  // two's complement wrap for negative frequencies
      // Realised frequency within half a uint32 step of the requested one.
      const double realised = static_cast<double>(e) * kSr / 4294967296.0;
      CHECK(std::abs(realised - static_cast<double>(hzPat[l])) <= 0.5 * kSr / 4294967296.0 + 1e-12);
    }

    uint64_t n = 0;
    uint64_t checkedSamples = 0;
    bool allOk = true;
    uint32_t blockIndex = 0;
    while (n < kTotal) {
      const uint32_t frames = static_cast<uint32_t>(std::min<uint64_t>(kBlock, kTotal - n));
      pq::dsp::KernelArgs a{ins, outs, &st, nullptr, frames, 2, 1, 0b11};
      fn(a);
      // Check the first frame of every block, and every frame of every 4096th block.
      const uint32_t checkFrames = (blockIndex % 4096 == 0) ? frames : 1;
      for (uint32_t f = 0; f < checkFrames; ++f)
        for (uint32_t l = 0; l < lanes; ++l) {
          const uint32_t expect = static_cast<uint32_t>(static_cast<uint64_t>(inc[l]) * (n + f));
          if (out[static_cast<size_t>(f) * lanes + l] != unitPhase(expect)) allOk = false;
          ++checkedSamples;
        }
      n += frames;
      ++blockIndex;
    }
    CHECK(allOk);
    CHECK(checkedSamples > kTotal / kBlock);
    for (uint32_t l = 0; l < lanes; ++l) {
      // Exact: (inc · 10^9) mod 2^32.
      const uint32_t expect = static_cast<uint32_t>(static_cast<uint64_t>(inc[l]) * kTotal);
      CHECK(st.phase[l] == expect);
    }
  }
}

TEST_CASE("phasor: audio-rate frequency path accumulates exactly too") {
  // A frequency sweep through the per-sample (Blk) path: the phase is the exact integer sum of the
  // per-sample increments.
  constexpr uint32_t kSr = 44100;
  constexpr uint32_t kFrames = 200000;
  for (uint32_t lanes : {1u, 8u}) {
    pq::dsp::StateInit init;
    init.sampleRate = kSr;
    init.lanes = lanes;
    pq::dsp::PhasorState st{};
    pq::dsp::initState(pq::dsp::Atom::Phasor, &st, init);
    std::vector<float> hz(static_cast<size_t>(kFrames) * lanes);
    for (uint32_t t = 0; t < kFrames; ++t)
      for (uint32_t l = 0; l < lanes; ++l)
        hz[static_cast<size_t>(t) * lanes + l] = static_cast<float>(30.0 + 5000.0 * (t % 1000) / 1000.0 + 17.0 * l);
    std::vector<float> out(hz.size());
    alignas(32) float resetPat[8] = {};
    const float* ins[] = {hz.data(), resetPat};
    float* outs[] = {out.data()};
    pq::dsp::KernelArgs a{ins, outs, &st, nullptr, kFrames, 2, 1, 0b10};
    pq::dsp::findKernel(pq::dsp::Atom::Phasor, lanes, 0b10)(a);
    for (uint32_t l = 0; l < lanes; ++l) {
      uint32_t ph = 0;
      bool ok = true;
      for (uint32_t t = 0; t < kFrames; ++t) {
        if (out[static_cast<size_t>(t) * lanes + l] != unitPhase(ph)) ok = false;
        ph += static_cast<uint32_t>(exactIncrement(hz[static_cast<size_t>(t) * lanes + l], kSr));
      }
      CHECK(ok);
      CHECK(st.phase[l] == ph);
    }
  }
}
