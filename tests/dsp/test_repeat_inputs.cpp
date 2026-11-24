// OP.6: the unit conversions computed in double (cents ↔ Hz, dB ↔ gain) reuse the previous vector's result when a
// vector of 8 inputs repeats the previous one bit for bit (kernels_common.h elementwiseMemo). The output must be the
// bits of the same function on the same values, whatever the input does: here every block is compared with the
// kernel called one vector at a time (a call's first vector is always computed) and with the same block computed in
// place (as the JIT runs it), over inputs that hold, start moving, move in one lane only, stop, and change only in a
// NaN payload or the sign of a zero. Both lane counts, every Blk / Spl mask, block lengths with a 1-lane remainder.
// The other exact transcendentals (pow, exp, log, tan: the plain driver) run through the same comparison.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/dsp/atoms.h"

using pq::dsp::Atom;

namespace {

constexpr Atom kMemo[] = {Atom::CentsToHz, Atom::HzToCents, Atom::Pow, Atom::Exp,
                          Atom::Log,       Atom::Tan,       Atom::DbToGain, Atom::GainToDb};

uint32_t rng(uint32_t& s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

// A value in the atom's working range for input `i` (cents / Hz / exponents / dB / radians).
float sample(Atom a, uint32_t i, uint32_t& s) {
  const float u = static_cast<float>(rng(s) >> 8) * (1.0f / 16777216.0f);  // [0, 1)
  switch (a) {
    case Atom::CentsToHz: return i == 0 ? -6000.0f + 12000.0f * u : 20.0f + 880.0f * u;
    case Atom::HzToCents: return 20.0f + 8000.0f * u;
    case Atom::Pow: return i == 0 ? 0.01f + 8.0f * u : -3.0f + 6.0f * u;
    case Atom::Exp: return -20.0f + 30.0f * u;
    case Atom::Log: return 1e-6f + 100.0f * u;
    case Atom::Tan: return -1.5f + 3.0f * u;
    case Atom::DbToGain: return -96.0f + 108.0f * u;
    default: return 1e-5f + 4.0f * u;  // GainToDb
  }
}

// `count` floats of input i: stretches that hold a vector of 8, stretches that move every value, single lanes
// changing, and special changes (NaN payloads, ±0, infinities) that an == compare would miss.
std::vector<float> pattern(Atom a, uint32_t i, uint32_t count, uint32_t seed) {
  std::vector<float> v(count);
  uint32_t s = seed * 2654435761u + i * 40503u + 1u;
  float hold[8];
  for (float& h : hold) h = sample(a, i, s);
  const uint32_t vectors = count / 8;
  for (uint32_t k = 0; k < vectors; ++k) {
    const uint32_t phase = (k + seed) % 24;
    if (phase >= 6 && phase < 10) {  // moving: every value new
      for (float& h : hold) h = sample(a, i, s);
    } else if (phase == 12) {  // one lane steps
      hold[rng(s) & 7u] = sample(a, i, s);
    } else if (phase == 14) {  // a zero, then its sign
      hold[3] = 0.0f;
    } else if (phase == 15) {
      hold[3] = -0.0f;
    } else if (phase == 17) {  // a NaN, then another payload
      hold[5] = std::bit_cast<float>(0x7FC00000u);
    } else if (phase == 18) {
      hold[5] = std::bit_cast<float>(0x7FC00001u);
    } else if (phase == 20) {
      hold[1] = INFINITY;
    } else if (phase == 21) {
      hold[1] = sample(a, i, s);
      hold[5] = sample(a, i, s);
      hold[3] = sample(a, i, s);
    }
    std::memcpy(v.data() + static_cast<size_t>(k) * 8, hold, sizeof hold);
  }
  for (uint32_t k = vectors * 8; k < count; ++k) v[k] = sample(a, i, s);
  return v;
}

struct Run {
  uint64_t values = 0, differ = 0;
  std::string first;
};

// One block of `frames` at `lanes` with Spl mask `mask` against the same kernel called a vector at a time.
void compare(Atom a, uint32_t lanes, uint32_t mask, uint32_t frames, uint32_t seed, Run& r) {
  const pq::DenormalGuard dg;
  const uint32_t nIn = pq::dsp::kernelInfo(a).numIn;
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, lanes, mask);
  REQUIRE(fn != nullptr);
  const uint32_t count = frames * lanes;
  std::vector<float> in[2];
  const float* inp[2]{};
  for (uint32_t i = 0; i < nIn; ++i) {
    const bool spl = ((mask >> i) & 1u) != 0;
    in[i] = pattern(a, i, spl ? 8 : count, seed + 97 * i);
    if (spl && lanes == 1) in[i].assign(8, in[i][0]);  // a 1-lane Spl pattern is its value 8 times
    inp[i] = in[i].data();
  }
  std::vector<float> out(count, -1.0f), ref(count, -2.0f);
  float* outp[1] = {out.data()};
  {
    const pq::RtScope rt;  // the kernel is RT code: no allocation in the memo path either
    fn(pq::dsp::KernelArgs{inp, outp, nullptr, nullptr, frames, static_cast<uint16_t>(nIn), 1, mask});
  }
  // In place (the output over a block input: the previous input vector is gone when the next one is compared).
  for (uint32_t i = 0; i < nIn; ++i) {
    if (((mask >> i) & 1u) != 0 || count == 0) continue;
    std::vector<float> io = in[i];
    const float* ip[2] = {inp[0], inp[1]};
    ip[i] = io.data();
    float* op[1] = {io.data()};
    fn(pq::dsp::KernelArgs{ip, op, nullptr, nullptr, frames, static_cast<uint16_t>(nIn), 1, mask});
    if (std::memcmp(io.data(), out.data(), count * sizeof(float)) != 0 && r.differ++ == 0)
      r.first = std::string(pq::dsp::kernelInfo(a).name) + " in place over input " + std::to_string(i) + ", lanes " +
                std::to_string(lanes) + " mask " + std::to_string(mask) + " frames " + std::to_string(frames);
  }
  // The reference: 8 floats per call (1 frame of 8 lanes, 8 frames of 1 lane), then the 1-lane remainder.
  const uint32_t step = 8 / lanes;
  for (uint32_t f = 0; f < frames; f += step) {
    const uint32_t n = std::min(step, frames - f);
    const float* rin[2]{};
    for (uint32_t i = 0; i < nIn; ++i) rin[i] = ((mask >> i) & 1u) ? inp[i] : inp[i] + static_cast<size_t>(f) * lanes;
    float* rout[1] = {ref.data() + static_cast<size_t>(f) * lanes};
    fn(pq::dsp::KernelArgs{rin, rout, nullptr, nullptr, n, static_cast<uint16_t>(nIn), 1, mask});
  }
  for (uint32_t k = 0; k < count; ++k) {
    ++r.values;
    if (std::bit_cast<uint32_t>(out[k]) == std::bit_cast<uint32_t>(ref[k])) continue;
    if (r.differ++ == 0)
      r.first = std::string(pq::dsp::kernelInfo(a).name) + " lanes " + std::to_string(lanes) + " mask " +
                std::to_string(mask) + " frames " + std::to_string(frames) + " at " + std::to_string(k);
  }
}

}  // namespace

TEST_CASE("dsp (OP.6): a repeated input vector reuses its result — bit-identical to one vector per call") {
  Run r;
  for (Atom a : kMemo) {
    const uint32_t nIn = pq::dsp::kernelInfo(a).numIn;
    for (uint32_t mask = 0; mask < (1u << nIn); ++mask)
      for (uint32_t seed = 0; seed < 24; ++seed) {
        compare(a, 8, mask, 64, seed, r);
        compare(a, 8, mask, 1 + seed % 5, seed, r);
        compare(a, 1, mask, 512, seed, r);
        compare(a, 1, mask, 64 + seed % 8, seed, r);  // a remainder below 8 frames
        compare(a, 1, mask, seed % 8, seed, r);       // only the remainder (and the empty block)
      }
  }
  CHECK_MESSAGE(r.differ == 0, r.differ << " of " << r.values << " differ; first: " << r.first);
  MESSAGE("compared " << r.values << " results");
}

// The case the fast path is for, and its edges: a held block, the block in which the input starts to move, and the
// block in which it stops, each equal to the per-vector result (cents → Hz as a voice's pitch chain runs it).
TEST_CASE("dsp (OP.6): cents → Hz of a held, a starting and a stopping glide equals the per-frame result") {
  const pq::DenormalGuard dg;
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::CentsToHz, 8, 2);  // cents per sample, a param-rate reference
  REQUIRE(fn != nullptr);
  constexpr uint32_t kFrames = 64;
  alignas(32) float ref[8];
  for (float& v : ref) v = 261.6256f;
  auto block = [&](uint32_t moveFrom, uint32_t moveTo) {  // lanes 2 and 5 glide over frames [moveFrom, moveTo)
    std::vector<float> cents(kFrames * 8);
    float c[8] = {0.0f, 700.0f, -1200.0f, 386.3137f, 1901.955f, 33.0f, -0.0f, 1200.0f};
    for (uint32_t f = 0; f < kFrames; ++f) {
      if (f >= moveFrom && f < moveTo) {
        c[2] += 3.125f;
        c[5] -= 0.71f;
      }
      std::memcpy(cents.data() + static_cast<size_t>(f) * 8, c, sizeof c);
    }
    std::vector<float> out(kFrames * 8), one(kFrames * 8);
    const float* in[2] = {cents.data(), ref};
    float* o[1] = {out.data()};
    fn(pq::dsp::KernelArgs{in, o, nullptr, nullptr, kFrames, 2, 1, 2});
    for (uint32_t f = 0; f < kFrames; ++f) {
      const float* in1[2] = {cents.data() + static_cast<size_t>(f) * 8, ref};
      float* o1[1] = {one.data() + static_cast<size_t>(f) * 8};
      fn(pq::dsp::KernelArgs{in1, o1, nullptr, nullptr, 1, 2, 1, 2});
    }
    CHECK(std::memcmp(out.data(), one.data(), out.size() * sizeof(float)) == 0);
    return out;
  };
  const std::vector<float> held = block(0, 0);
  for (uint32_t f = 1; f < kFrames; ++f) CHECK(std::memcmp(held.data(), held.data() + static_cast<size_t>(f) * 8, 32) == 0);
  CHECK(held[1] == doctest::Approx(261.6256 * std::exp2(700.0 / 1200.0)).epsilon(1e-6));
  (void)block(40, kFrames);  // starts moving mid-block
  (void)block(0, 23);        // stops mid-block
  (void)block(17, 18);       // one frame's step
  (void)block(0, kFrames);   // moves throughout
}
