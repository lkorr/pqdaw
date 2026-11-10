// T6.5: the stretch pool (pq/engine/stretch_pool.h; docs/plans/M6.md T6.5):
//   - THE SPIKE: signalsmith-stretch's process() / seek() / reset never allocate on the audio thread: RtScope over 10k
//     blocks of varying size, time ratio (0.1× .. 8×), transpose (−2 .. +2 octaves) and formant settings, with claims
//     and releases between them
//   - claims: kNone when exhausted, released stretchers come back; reseeded claims render identically (deterministic)
//   - latency: the reported input / output latencies put a primed stretcher's output on its input (a click lands
//     within ±2 frames)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/engine/stretch_pool.h"

namespace {

namespace e = pq::engine;

constexpr double kSr = 48000.0;

struct Noise {
  uint32_t s = 12345u;
  float operator()() noexcept {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f;
  }
};

}  // namespace

TEST_CASE("T6.5 spike: signalsmith-stretch never allocates on the audio thread (10k blocks, varying ratio / pitch)") {
  REQUIRE(pq::alloc_trap::installed());
  e::StretchPool pool(2, kSr);
  REQUIRE(pool.size() == 2);
  std::vector<float> inL(1 << 16), inR(1 << 16), outL(4096), outR(4096);
  Noise nz;
  double ph = 0.0;
  for (size_t i = 0; i < inL.size(); ++i) {  // a tone with noise (peaks for the pitch map, energy everywhere)
    ph += 2.0 * std::numbers::pi * 330.0 / kSr;
    inL[i] = 0.5f * static_cast<float>(std::sin(ph)) + 0.1f * nz();
    inR[i] = 0.4f * static_cast<float>(std::sin(1.5 * ph)) + 0.1f * nz();
  }
  const float* in[2] = {inL.data(), inR.data()};
  float* out[2] = {outL.data(), outR.data()};
  uint64_t violations = 0;
  uint32_t rnd = 77u;
  auto next = [&](uint32_t m) {
    rnd = rnd * 1103515245u + 12345u;
    return (rnd >> 8) % m;
  };
  uint32_t idx = pool.claim(1);
  REQUIRE(idx != e::StretchPool::kNone);
  for (int block = 0; block < 10000; ++block) {
    const int outN = 16 + static_cast<int>(next(1009));                      // 16 .. 1024 output frames
    const double ratio = 0.1 * std::pow(80.0, next(1000) / 999.0);            // 0.1 .. 8 input frames per output frame
    const int inN = std::min(static_cast<int>(std::lround(outN * ratio)), 8192);
    const auto offset = static_cast<int>(next(static_cast<uint32_t>(inL.size()) - 8192));
    const float* src[2] = {in[0] + offset, in[1] + offset};
    e::StretchParams p;
    p.transpose = std::pow(2.0, (static_cast<double>(next(4801)) - 2400.0) / 1200.0);
    const uint32_t mode = next(4);
    p.compensate = mode >= 2;
    p.formant = mode == 3 ? std::pow(2.0, (static_cast<double>(next(1201)) - 600.0) / 1200.0) : 1.0;
    p.formantBase = mode == 2 ? 1.0 / (8.0 + next(249)) : 0.0;
    const uint32_t event = next(64);
    const int lead = next(16) == 0 ? inN / 2 : 0;
    const float* srcLead[2] = {src[0] + lead, src[1] + lead};
    violations += pq::alloc_trap::countRtViolations([&] {
      if (event == 0) {  // a voice ends and another starts: release, claim, prime
        pool.release(idx);
        idx = pool.claim(static_cast<uint32_t>(block));
        pool.setParams(idx, p);
        pool.seek(idx, src, pool.latency().seek, ratio);
      } else if (event == 1) {
        pool.setParams(idx, p);
      }
      pool.process(idx, srcLead, inN, lead, out, outN);
    });
    if (event == 2) {  // silence long enough for the library's pass-through branch
      violations += pq::alloc_trap::countRtViolations([&] {
        std::fill(outL.begin(), outL.end(), 0.0f);
        const float* z[2] = {outL.data(), outL.data()};
        for (int k = 0; k < 16; ++k) pool.process(idx, z, 1024, 0, out, 1024);
      });
    }
  }
  CHECK(violations == 0);
  for (float v : outL) REQUIRE(std::isfinite(v));
  pool.release(idx);
}

TEST_CASE("T6.5: the pool claims until exhausted; reseeded claims render identically") {
  e::StretchPool pool(2, kSr);
  CHECK(pool.latency().input > 0);
  CHECK(pool.latency().output > 0);
  CHECK(pool.memoryBytes() > 0);
  for (const double sr : {44100.0, 48000.0, 96000.0}) {  // the formula the prefetch positions use == the stretcher's
    const e::StretchPool p1(1, sr);
    CHECK(p1.latency() == e::StretchPool::latencyFor(sr));
  }
  const uint32_t a = pool.claim(5), b = pool.claim(5);
  CHECK(a != e::StretchPool::kNone);
  CHECK(b != e::StretchPool::kNone);
  CHECK(pool.claim(5) == e::StretchPool::kNone);
  CHECK(pool.available() == 0);
  // Both stretchers render a 3× stretch (phase randomisation on) of the same noise: identical.
  std::vector<float> in(48000);
  Noise nz;
  for (float& v : in) v = nz();
  const int seekN = pool.latency().seek;
  auto render = [&](uint32_t i) {
    std::vector<float> o(3 * 48000, 0.0f);
    const float* ip[2] = {in.data(), in.data()};
    pool.seek(i, ip, seekN, 1.0 / 3.0);
    for (int k = 0; seekN + (k + 1) * 160 <= 48000 && (k + 1) * 480 <= static_cast<int>(o.size()); ++k) {
      const float* src[2] = {in.data() + seekN + k * 160, in.data() + seekN + k * 160};
      float* out[2] = {o.data() + k * 480, o.data() + k * 480};
      pool.process(i, src, 160, 0, out, 480);
    }
    return o;
  };
  const std::vector<float> ra = render(a), rb = render(b);
  CHECK(ra == rb);
  CHECK(pool.generatorState(a) == pool.generatorState(b));
  CHECK(pool.generatorState(a) != (0x5EED5EED00000000ull ^ 5u));  // the generator was used
  pool.release(a);
  CHECK(pool.available() == 1);
  const uint32_t c = pool.claim(5);
  CHECK(c == a);
  CHECK(render(c) == ra);
  pool.release(b);
  pool.release(c);
}

TEST_CASE("T6.5: input / output latency: a primed stretcher's output lines up with its input") {
  e::StretchPool pool(1, kSr);
  const uint32_t i = pool.claim(0);
  const int inLat = pool.latency().input, outLat = pool.latency().output;
  // A click at input frame C. Output frame t should carry input frame origin + t − outLat (the analysis centre); the
  // head runs at centre(t + outLat) + inLat = origin + t + inLat, as the clip player primes and feeds a voice.
  constexpr int C = 30000;
  const int origin = 20000;
  std::vector<float> in(200000, 0.0f);
  in[C] = 1.0f;
  const float* seekIn[2] = {in.data() + origin + inLat - pool.latency().seek, in.data() + origin + inLat - pool.latency().seek};
  pool.seek(i, seekIn, pool.latency().seek, 1.0);
  std::vector<float> o(static_cast<size_t>(outLat) + 40000, 0.0f);
  int consumed = origin + inLat;
  for (int at = 0; at + 128 <= static_cast<int>(o.size()); at += 128) {
    const int target = origin + (at + 128) + inLat;  // the head at the end of this cell
    const float* src[2] = {in.data() + consumed, in.data() + consumed};
    float* out[2] = {o.data() + at, o.data() + at};
    pool.process(i, src, target - consumed, 0, out, 128);
    consumed = target;
  }
  const auto peak = std::max_element(o.begin(), o.end(), [](float x, float y) { return std::abs(x) < std::abs(y); }) - o.begin();
  MESSAGE("click at output " << peak << ", expected " << outLat + (C - origin) << " (inLat " << inLat << ", outLat "
                             << outLat << ")");
  CHECK(std::abs(static_cast<int>(peak) - (outLat + (C - origin))) <= 2);
  pool.release(i);
}
