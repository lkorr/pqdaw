// DY.1: the dynamics atoms (pq/dsp/atoms.h "Dynamics", kernels_dynamics.cpp).
//   follow   step response: 63 % of a step after one attack time constant, then 1/e after one release one (attack ≠
//            release); attack 0 tracks a rise exactly; Blk = Spl coefficients and 1 lane = 8 lanes bit for bit; a NaN /
//            inf input leaves the envelope finite
//   window   max / min / mean of the last L inputs equal a brute-force reference on random input for L ∈ {1, 7, 64,
//            1153} (max / min exactly, mean within float rounding), with the same bits for every block size and for
//            1 and 8 lanes; a voice lane reset (the initial words) or moved from another group (another phase) goes on
//            exactly as a fresh / its own 1-lane window would; NaN reads as 0
//   RtScope  both kernels, both lane counts, every Spl combination (one block each, also a realignment)
// DY.5:
//   truepeak = the BS.1770-4 phases in direct form (double) on random input, the same bits for every block size and for
//            1 and 8 lanes; causal (outputs to frame m read inputs to m), an impulse peaks exactly 6 frames later and
//            is gone 12; within 0.2 dB below / 0.23 dB above (Q?-DY.5-a) a 16× oversampled reference on 400 sines at
//            0.24–0.49·sr with random phases; RtScope
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"

using namespace pq::dsp;

namespace {

// One stateful kernel with its state; inputs per call are given as per-input vectors (Blk: frames · lanes floats,
// Spl: 8 floats).
struct Rig {
  Atom atom;
  uint32_t lanes;
  StateInit init;
  pq::AlignedBuffer<float> state;
  Rig(Atom a, uint32_t l, StateInit in) : atom(a), lanes(l), init(in), state(bytes(a, l, in) / 4 + 8) { reset(); }
  static size_t bytes(Atom a, uint32_t l, StateInit in) {
    in.lanes = l;
    return stateBytes(a, in);
  }
  void reset() {
    init.lanes = lanes;
    initState(atom, state.data(), init);
  }
  // Runs one call: `in[i]` holds the input (Blk if its size is frames · lanes, else a Spl pattern).
  std::vector<float> run(const std::vector<std::vector<float>>& in, uint32_t frames) {
    const KernelInfo& info = kernelInfo(atom);
    std::vector<const float*> p;
    uint32_t spl = 0;
    for (uint32_t i = 0; i < in.size(); ++i) {
      p.push_back(in[i].data());
      if (in[i].size() != static_cast<size_t>(frames) * lanes) spl |= 1u << i;
    }
    std::vector<float> out(static_cast<size_t>(frames) * lanes + 1, 0.0f);
    float* o = out.data();
    const KernelFn fn = findKernel(atom, lanes, spl);
    REQUIRE(fn != nullptr);
    KernelArgs args{p.data(), &o, state.data(), nullptr, frames, static_cast<uint16_t>(in.size()), 1, spl};
    fn(args);
    out.pop_back();
    (void)info;
    return out;
  }
};

std::vector<float> pattern(float v) { return std::vector<float>(8, v); }

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Runs a 1-input window over `x` (frames · lanes) in calls of `block` frames.
std::vector<float> runWindow(Rig& r, const std::vector<float>& x, uint32_t block) {
  const uint32_t frames = static_cast<uint32_t>(x.size() / r.lanes);
  std::vector<float> y;
  for (uint32_t f = 0; f < frames; f += block) {
    const uint32_t n = std::min(block, frames - f);
    std::vector<float> in(x.begin() + static_cast<size_t>(f) * r.lanes, x.begin() + static_cast<size_t>(f + n) * r.lanes);
    const std::vector<float> o = r.run({in}, n);
    y.insert(y.end(), o.begin(), o.end());
  }
  return y;
}

// Brute force over a zero history: out[t] = op over x[t − L + 1 … t] (x before 0 is 0); mean in double.
std::vector<double> bruteWindow(const std::vector<float>& x, uint32_t len, uint32_t op) {
  std::vector<double> y(x.size());
  for (size_t t = 0; t < x.size(); ++t) {
    double m = op == 0 ? -std::numeric_limits<double>::infinity() : op == 1 ? std::numeric_limits<double>::infinity() : 0.0;
    for (uint32_t k = 0; k < len; ++k) {
      const double v = t >= k ? static_cast<double>(x[t - k]) : 0.0;
      m = op == 0 ? std::max(m, v) : op == 1 ? std::min(m, v) : m + v;
    }
    y[t] = op == 2 ? m / len : m;
  }
  return y;
}

std::vector<float> lane(const std::vector<float>& v, uint32_t l, uint32_t lanes) {
  std::vector<float> o(v.size() / lanes);
  for (size_t f = 0; f < o.size(); ++f) o[f] = v[f * lanes + l];
  return o;
}

}  // namespace

TEST_CASE("follow: step response (63 % after one time constant, attack ≠ release), instant attack, lanes, rates") {
  pq::DenormalGuard dg;
  constexpr float kA = 480.0f, kR = 4800.0f;  // 10 ms / 100 ms at 48 kHz, in samples
  for (uint32_t lanes : {1u, 8u}) {
    INFO("lanes " << lanes);
    Rig r(Atom::Follow, lanes, {});
    // A step 0 → 1 for 3·kA frames, then back to 0.
    const uint32_t up = 3 * static_cast<uint32_t>(kA), down = 2 * static_cast<uint32_t>(kR);
    std::vector<float> x1(static_cast<size_t>(up) * lanes, 1.0f), x0(static_cast<size_t>(down) * lanes, 0.0f);
    const std::vector<float> yu = r.run({x1, pattern(kA), pattern(kR)}, up);
    const std::vector<float> yd = r.run({x0, pattern(kA), pattern(kR)}, down);
    for (uint32_t l = 0; l < lanes; ++l) {
      // After kA frames: 1 − e^−1 (the attack); kR frames into the fall: e^−1 of where it started (the release).
      CHECK(yu[(static_cast<size_t>(kA) - 1) * lanes + l] == doctest::Approx(1.0 - std::exp(-1.0)).epsilon(2e-3));
      const double start = yu[(static_cast<size_t>(up) - 1) * lanes + l];
      CHECK(yd[(static_cast<size_t>(kR) - 1) * lanes + l] == doctest::Approx(start * std::exp(-1.0)).epsilon(2e-3));
      // Release ≠ attack: kA frames into the fall it has dropped only to e^(−kA/kR) (≈ 0.905), not e^−1.
      CHECK(yd[(static_cast<size_t>(kA) - 1) * lanes + l] == doctest::Approx(start * std::exp(-kA / kR)).epsilon(2e-3));
    }
    // Attack 0: a rising input is tracked exactly; a fall still releases.
    Rig r0(Atom::Follow, lanes, {});
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<float> x(static_cast<size_t>(500) * lanes);
    for (float& v : x) v = u(rng);
    const std::vector<float> y = r0.run({x, pattern(0.0f), pattern(200.0f)}, 500);
    float prev = 0.0f;
    for (uint32_t f = 0; f < 500; ++f) {
      const float xf = x[static_cast<size_t>(f) * lanes], yf = y[static_cast<size_t>(f) * lanes];
      if (xf > prev) CHECK(yf == xf);
      else CHECK(yf >= xf - 1e-6f);  // a release: a convex combination of y ≥ x and x (within rounding)
      prev = yf;
    }
  }
  // Blk coefficients = Spl ones (same exp) and 8 lanes = 1 lane, bit for bit, in calls of odd sizes; NaN / inf stay out.
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  constexpr uint32_t kN = 997;
  std::vector<float> x(kN);
  for (float& v : x) v = u(rng);
  x[100] = std::numeric_limits<float>::quiet_NaN();
  x[300] = std::numeric_limits<float>::infinity();
  x[301] = -std::numeric_limits<float>::infinity();
  std::vector<float> ref;
  {
    Rig r(Atom::Follow, 1, {});
    ref = r.run({x, pattern(30.0f), pattern(900.0f)}, kN);
  }
  for (float v : ref) REQUIRE(std::isfinite(v));
  for (uint32_t lanes : {1u, 8u}) {
    Rig r(Atom::Follow, lanes, {});
    for (uint32_t f = 0, b = 1; f < kN; f += b, b = b * 3 % 61 + 1) {
      const uint32_t n = std::min(b, kN - f);
      std::vector<float> xi, ai, ri;
      for (uint32_t k = 0; k < n; ++k)
        for (uint32_t l = 0; l < lanes; ++l) {
          xi.push_back(x[f + k]);
          ai.push_back(30.0f);
          ri.push_back(900.0f);
        }
      const bool blkA = (f / 7) % 2 == 0, blkR = (f / 11) % 2 == 0;
      const std::vector<float> o = r.run({xi, blkA ? ai : pattern(30.0f), blkR ? ri : pattern(900.0f)}, n);
      for (uint32_t k = 0; k < n; ++k)
        for (uint32_t l = 0; l < lanes; ++l) REQUIRE(std::memcmp(&o[k * lanes + l], &ref[f + k], 4) == 0);
    }
  }
}

TEST_CASE("window: max / min / mean = brute force for L 1, 7, 64, 1153; same bits for any block size and lane count") {
  pq::DenormalGuard dg;
  std::mt19937 rng(20261113);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (uint32_t len : {1u, 7u, 64u, 1153u}) {
    const uint32_t frames = 3 * len + 700;
    std::vector<float> x(frames);
    for (float& v : x) v = u(rng);
    for (uint32_t op = 0; op < 3; ++op) {
      INFO("L " << len << ", op " << op);
      const std::vector<double> want = bruteWindow(x, len, op);
      StateInit init;
      init.windowFrames = len;
      init.windowOp = op;
      std::vector<float> first;
      for (uint32_t lanes : {1u, 8u})
        for (uint32_t block : {1u, 13u, 64u, 512u, frames}) {
          INFO("lanes " << lanes << ", block " << block);
          Rig r(Atom::Window, lanes, init);
          // 8 lanes: lane l reads x scaled by (1 + l / 8) (every lane its own data; lane 0 is x).
          std::vector<float> xi(static_cast<size_t>(frames) * lanes);
          for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t l = 0; l < lanes; ++l) xi[static_cast<size_t>(f) * lanes + l] = l == 0 ? x[f] : x[f] * (1.0f + l / 8.0f);
          const std::vector<float> y = runWindow(r, xi, block);
          const std::vector<float> y0 = lane(y, 0, lanes);
          if (first.empty()) {
            first = y0;
            for (uint32_t f = 0; f < frames; ++f) {
              INFO("frame " << f);
              if (op < 2) REQUIRE(static_cast<double>(y0[f]) == want[f]);
              else REQUIRE(std::fabs(static_cast<double>(y0[f]) - want[f]) <= 1e-6);
            }
          } else {
            REQUIRE(sameBits(y0, first));
          }
          if (lanes == 8 && block == 13) {  // the other lanes against 1-lane windows of their own data
            for (uint32_t l = 1; l < 8; ++l) {
              Rig r1(Atom::Window, 1, init);
              const std::vector<float> ref = runWindow(r1, lane(xi, l, 8), 64);
              CHECK(sameBits(lane(y, l, 8), ref));
            }
          }
        }
    }
  }
  // A param-rate (Spl) x: a constant window; NaN reads as 0.
  StateInit init;
  init.windowFrames = 5;
  init.windowOp = 1;
  Rig r(Atom::Window, 1, init);
  const std::vector<float> y = r.run({pattern(std::numeric_limits<float>::quiet_NaN())}, 3);
  for (float v : y) CHECK(v == 0.0f);
  const std::vector<float> y2 = r.run({pattern(2.0f)}, 6);
  CHECK(y2[3] == 0.0f);  // the last NaN-zero (frame 2) is still in the window of frames 2 … 6
  CHECK(y2[4] == 2.0f);
  for (uint32_t lanes : {1u, 8u}) {  // the mean of a param-rate x: 2 entering a zero history of 4
    StateInit mi;
    mi.windowFrames = 4;
    mi.windowOp = 2;
    Rig m(Atom::Window, lanes, mi);
    const std::vector<float> ym = m.run({pattern(2.0f)}, 6);
    for (uint32_t f = 0; f < 6; ++f) CHECK(ym[static_cast<size_t>(f) * lanes] == std::min(2.0f, 0.5f * static_cast<float>(f + 1)));
  }
}

TEST_CASE("window: a voice lane reset or moved from another group's phase goes on as its own window") {
  pq::DenormalGuard dg;
  std::mt19937 rng(4242);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (uint32_t op = 0; op < 3; ++op)
    for (uint32_t len : {7u, 64u}) {
      INFO("op " << op << ", L " << len);
      StateInit init;
      init.windowFrames = len;
      init.windowOp = op;
      init.lanes = 8;
      const size_t words = stateBytes(Atom::Window, init) / 4;
      // Two groups at different phases: A has run 100 frames, B 100 + 23; per-lane histories in 1-lane mirrors.
      Rig a(Atom::Window, 8, init), b(Atom::Window, 8, init);
      std::vector<Rig> mirA, mirB;
      for (uint32_t l = 0; l < 8; ++l) {
        mirA.emplace_back(Atom::Window, 1, init);
        mirB.emplace_back(Atom::Window, 1, init);
      }
      auto feed = [&](Rig& g, std::vector<Rig>& mir, uint32_t frames) {
        std::vector<float> x(static_cast<size_t>(frames) * 8);
        for (float& v : x) v = u(rng);
        const std::vector<float> y = g.run({x}, frames);
        for (uint32_t l = 0; l < 8; ++l) {
          const std::vector<float> want = mir[l].run({lane(x, l, 8)}, frames);
          REQUIRE(lane(y, l, 8) == want);
        }
      };
      feed(a, mirA, 100);
      feed(b, mirB, 123);
      // Lane 2 of A: reset (the initial words of that lane, as the VoiceEngine's Whole reset); lane 5 of A: replaced
      // by lane 6 of B (a move). Every word is per lane: word w belongs to lane w mod 8.
      Rig fresh(Atom::Window, 8, init);
      auto* sa = reinterpret_cast<uint32_t*>(a.state.data());
      const auto* sb = reinterpret_cast<const uint32_t*>(b.state.data());
      const auto* sf = reinterpret_cast<const uint32_t*>(fresh.state.data());
      for (size_t w = 0; w < words; w += 8) {
        sa[w + 2] = sf[w + 2];
        sa[w + 5] = sb[w + 6];
      }
      mirA[2].reset();
      std::swap(mirA[5], mirB[6]);
      // Then A goes on: lane 2 as a fresh window, lane 5 as B's lane 6, the others unchanged (max / min bit for bit;
      // the mean's ring sum restarts in another order, so within rounding).
      for (int call = 0; call < 3; ++call) {
        const uint32_t frames = call == 0 ? 5u : 2 * len + 3;
        std::vector<float> x(static_cast<size_t>(frames) * 8);
        for (float& v : x) v = u(rng);
        const std::vector<float> y = a.run({x}, frames);
        for (uint32_t l = 0; l < 8; ++l) {
          INFO("lane " << l << ", call " << call);
          const std::vector<float> want = mirA[l].run({lane(x, l, 8)}, frames);
          const std::vector<float> got = lane(y, l, 8);
          for (uint32_t f = 0; f < frames; ++f) {
            if (op < 2) REQUIRE(got[f] == want[f]);
            else REQUIRE(std::fabs(got[f] - want[f]) <= 1e-6f);
          }
        }
      }
    }
}

TEST_CASE("follow / window: RtScope (both lane counts, every Spl combination, a re-alignment)") {
  pq::DenormalGuard dg;
  constexpr uint32_t kFrames = 700;
  std::vector<float> blk(static_cast<size_t>(kFrames) * 8, 0.25f), blkT(static_cast<size_t>(kFrames) * 8, 40.0f);
  std::vector<float> out(static_cast<size_t>(kFrames) * 8);
  const std::vector<float> splX = pattern(0.5f), splT = pattern(40.0f);
  uint64_t violations = 0, calls = 0;
  for (Atom atom : {Atom::Follow, Atom::Window})
    for (uint32_t lanes : {1u, 8u})
      for (uint32_t op = 0; op < 3; ++op) {
        StateInit init;
        init.windowFrames = 300;
        init.windowOp = op;
        Rig r(atom, lanes, init);
        const uint32_t nIn = kernelInfo(atom).numIn;
        for (uint32_t spl = 0; spl < (1u << nIn); ++spl) {
          const float* in[3];
          for (uint32_t i = 0; i < nIn; ++i)
            in[i] = (spl >> i & 1u) ? (i == 0 ? splX.data() : splT.data()) : (i == 0 ? blk.data() : blkT.data());
          float* o = out.data();
          if (atom == Atom::Window && lanes == 8) reinterpret_cast<WindowState*>(r.state.data())->pos[3] = 17;  // misaligned
          const KernelFn fn = findKernel(atom, lanes, spl);
          REQUIRE(fn != nullptr);
          const KernelArgs args{in, &o, r.state.data(), nullptr, kFrames, static_cast<uint16_t>(nIn), 1, spl};
          violations += pq::alloc_trap::countRtViolations([&] { fn(args); });
          ++calls;
        }
      }
  CHECK(calls == 2 * 3 * (8 + 2));
  CHECK(violations == 0);
}

// ---- truepeak (DY.5) -------------------------------------------------------------------------------------------------

namespace {

// ITU-R BS.1770-4 Annex 2, typed again from the standard (the kernel derives sum / difference rows from its own copy).
constexpr double kBs1770[4][12] = {
    {0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
     0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500},
    {-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
     0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375},
    {-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500,
     0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875},
    {-0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500,
     0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750},
};

// Direct form in double over a zero history: max(|x[t − 6]|, |Σ_j h_p[j] x[t − j]| for the 4 phases).
std::vector<double> bruteTruePeak(const std::vector<float>& x) {
  std::vector<double> y(x.size());
  auto at = [&](size_t t, size_t k) { return t >= k ? static_cast<double>(x[t - k]) : 0.0; };
  for (size_t t = 0; t < x.size(); ++t) {
    double m = std::fabs(at(t, 6));
    for (const auto& h : kBs1770) {
      double acc = 0.0;
      for (size_t j = 0; j < 12; ++j) acc += h[j] * at(t, j);
      m = std::max(m, std::fabs(acc));
    }
    y[t] = m;
  }
  return y;
}

std::vector<float> truePeakIn(const std::vector<float>& x, uint32_t lanes, uint32_t block) {
  Rig r(Atom::TruePeak, lanes, StateInit{});
  return runWindow(r, x, block);  // any 1-input kernel in calls of `block` frames
}

}  // namespace

TEST_CASE("truepeak: = the BS.1770 phases in direct form; same bits for any block size and for 1 and 8 lanes") {
  pq::DenormalGuard dg;
  constexpr uint32_t kN = 1000;
  std::mt19937 rng(1770);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> x8(static_cast<size_t>(kN) * 8);
  for (float& v : x8) v = u(rng) * (u(rng) > 0.9f ? 4.0f : 1.0f);
  const std::vector<float> ref8 = truePeakIn(x8, 8, kN);
  for (uint32_t block : {1u, 7u, 8u, 11u, 12u, 64u, 333u}) {
    INFO("8 lanes, block " << block);
    CHECK(sameBits(truePeakIn(x8, 8, block), ref8));
  }
  for (uint32_t l = 0; l < 8; ++l) {
    INFO("lane " << l);
    const std::vector<float> x = lane(x8, l, 8);
    const std::vector<float> y = truePeakIn(x, 1, kN);
    CHECK(sameBits(y, lane(ref8, l, 8)));
    if (l < 2)
      for (uint32_t block : {1u, 3u, 5u, 8u, 13u, 16u, 17u, 64u, 100u}) {
        INFO("1 lane, block " << block);
        CHECK(sameBits(truePeakIn(x, 1, block), y));
      }
    const std::vector<double> b = bruteTruePeak(x);
    double worst = 0.0;
    for (uint32_t t = 0; t < kN; ++t) worst = std::max(worst, std::fabs(y[t] - b[t]) / std::max(1.0, b[t]));
    CHECK(worst < 2e-6);
  }
}

TEST_CASE("truepeak: causal, an impulse peaks exactly 6 frames later, silent 11 frames after the input ends") {
  constexpr uint32_t kN = 100, kAt = 37;
  for (uint32_t lanes : {1u, 8u}) {
    INFO("lanes " << lanes);
    std::vector<float> x(static_cast<size_t>(kN) * lanes, 0.0f);
    for (uint32_t l = 0; l < lanes; ++l) x[kAt * lanes + l] = l % 2 ? -0.5f : 0.5f;
    const std::vector<float> y = truePeakIn(x, lanes, 13);
    for (uint32_t l = 0; l < lanes; ++l) {
      const std::vector<float> yl = lane(y, l, lanes);
      for (uint32_t t = 0; t < kN; ++t) {
        INFO("frame " << t << " = " << yl[t]);
        if (t < kAt || t > kAt + 11) CHECK(yl[t] == 0.0f);
        else if (t == kAt + 6) CHECK(yl[t] == 0.5f);  // the sample itself (the phases' largest tap is 0.972)
        else CHECK(yl[t] < 0.5f * 0.9722f);
      }
      CHECK(yl[kAt + 5] == 0.5f * 0.9721679687500f);  // phase 3's centre tap, a quarter sample before
    }
  }
  // Outputs up to frame m depend on inputs up to m only.
  std::mt19937 rng(6);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> a(500), b;
  for (float& v : a) v = u(rng);
  for (uint32_t m : {0u, 1u, 11u, 100u, 257u}) {
    b = a;
    for (size_t t = m; t < b.size(); ++t) b[t] = u(rng);
    const std::vector<float> ya = truePeakIn(a, 1, 37), yb = truePeakIn(b, 1, 37);
    INFO("changed from " << m);
    CHECK(std::memcmp(ya.data(), yb.data(), m * sizeof(float)) == 0);
  }
}

// Q?-DY.5-a: the standard's own passband ripple (phases 1 and 2: +0.2 dB near sr/4) makes it OVER-read by up to
// ≈ 0.22 dB there; it never under-reads by more than a few thousandths of a dB on these. The bound is one-sided at 0.2.
TEST_CASE("truepeak: within 0.2 dB below / 0.23 dB above a 16× oversampled reference, sines 0.24–0.49·sr, random phases") {
  pq::DenormalGuard dg;
  constexpr uint32_t kN = 2048, kRounds = 50, kSkip = 64;
  std::mt19937 rng(20261113);
  std::uniform_real_distribution<double> uf(0.24, 0.49), uph(0.0, 2.0 * 3.141592653589793), ua(0.05, 1.0);
  double under = 0.0, over = 0.0;
  for (uint32_t round = 0; round < kRounds; ++round) {
    double f[8], ph[8], amp[8];
    std::vector<float> x(static_cast<size_t>(kN) * 8);
    for (uint32_t l = 0; l < 8; ++l) {
      f[l] = uf(rng), ph[l] = uph(rng), amp[l] = ua(rng);
      for (uint32_t t = 0; t < kN; ++t)
        x[static_cast<size_t>(t) * 8 + l] = static_cast<float>(amp[l] * std::sin(2.0 * 3.141592653589793 * f[l] * t + ph[l]));
    }
    const std::vector<float> y = truePeakIn(x, 8, 128);
    for (uint32_t l = 0; l < 8; ++l) {
      // The estimate at frames n ≥ kSkip covers positions n − 6 … n − 5.125; the reference is the band-limited
      // reconstruction of the same samples at 16× over that span — for a sine below Nyquist, the sine itself.
      float est = 0.0f;
      for (uint32_t t = kSkip; t < kN; ++t) est = std::max(est, y[static_cast<size_t>(t) * 8 + l]);
      double ref = 0.0;
      for (uint32_t k = (kSkip - 6) * 16; k < (kN - 5) * 16; ++k)
        ref = std::max(ref, std::fabs(amp[l] * std::sin(2.0 * 3.141592653589793 * f[l] * (k / 16.0) + ph[l])));
      const double db = 20.0 * std::log10(est / ref);
      INFO("f " << f[l] << "·sr, phase " << ph[l] << ": " << db << " dB");
      CHECK(db >= -0.2);
      CHECK(db <= 0.23);
      under = std::min(under, db), over = std::max(over, db);
    }
  }
  MESSAGE("truepeak vs 16x reference over 400 sines: " << under << " … +" << over << " dB");
}

TEST_CASE("truepeak: RtScope (both lane counts, short and long calls)") {
  pq::DenormalGuard dg;
  std::vector<float> x(700 * 8, 0.25f), out(700 * 8);
  uint64_t violations = 0;
  for (uint32_t lanes : {1u, 8u}) {
    Rig r(Atom::TruePeak, lanes, StateInit{});
    const KernelFn fn = findKernel(Atom::TruePeak, lanes, 0);
    REQUIRE(fn != nullptr);
    CHECK(findKernel(Atom::TruePeak, lanes, 1) == nullptr);  // x is always a block (the scheduler fills a Spl one)
    for (uint32_t frames : {1u, 5u, 13u, 700u}) {
      const float* in[1] = {x.data()};
      float* o = out.data();
      const KernelArgs args{in, &o, r.state.data(), nullptr, frames, 1, 1, 0};
      violations += pq::alloc_trap::countRtViolations([&] { fn(args); });
    }
  }
  CHECK(violations == 0);
}
