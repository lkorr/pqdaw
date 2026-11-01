// TX.smooth: the param_ramp kernel (pq/dsp/atoms.h "Param smoothing", kernels_ramp.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"

using namespace pq::dsp;

namespace {

struct Ramp {
  uint32_t lanes;
  float frames;
  pq::AlignedBuffer<float> state{sizeof(ParamRampState) / 4};
  explicit Ramp(uint32_t l, float f) : lanes(l), frames(f) {
    StateInit init;
    init.lanes = l;
    initState(Atom::ParamRamp, state.data(), init);
  }
  ParamRampState& st() { return *reinterpret_cast<ParamRampState*>(state.data()); }
  // One call over `n` frames with lane values x (lanes 8: per lane; 1: x[0]); returns whether it wrote `out`.
  bool run(const float* x8, uint32_t n, std::vector<float>& out) {
    alignas(32) float xs[8], fr[8];
    for (int l = 0; l < 8; ++l) xs[l] = x8[l], fr[l] = frames;
    out.assign(static_cast<size_t>(n) * lanes, -123.0f);
    const float* in[2] = {xs, fr};
    float* outs[1] = {out.data()};
    KernelArgs a;
    a.in = in;
    a.out = outs;
    a.state = state.data();
    a.frames = n;
    a.numIn = 2;
    a.numOut = 1;
    a.splMask = 3;
    KernelFn f = findKernel(Atom::ParamRamp, lanes, 3);
    REQUIRE(f != nullptr);
    CHECK(pq::alloc_trap::countRtViolations([&] { f(a); }) == 0);
    return st().active != 0;
  }
  bool run1(float x, uint32_t n, std::vector<float>& out) {
    const float x8[8] = {x, x, x, x, x, x, x, x};
    return run(x8, n, out);
  }
};

}  // namespace

TEST_CASE("param_ramp: starts at its input, ramps linearly to a change, restarts from where it is, settles exactly") {
  Ramp r(1, 100.0f);
  std::vector<float> out;
  CHECK_FALSE(r.run1(0.5f, 64, out));  // first call: starts there, nothing written
  CHECK(out[0] == -123.0f);
  CHECK_FALSE(r.run1(0.5f, 64, out));  // resting
  CHECK(r.run1(1.5f, 64, out));        // a step of 1 over 100 frames
  for (uint32_t k = 0; k < 64; ++k) CHECK(out[k] == doctest::Approx(0.5 + (k + 1) / 100.0).epsilon(1e-6));
  // A new target at frame 64 of the ramp: from where it is (1.14) to −1 over 100 frames, continuous.
  const float where = out[63];
  CHECK(r.run1(-1.0f, 64, out));
  CHECK(out[0] == doctest::Approx(where + (-1.0 - where) / 100.0).epsilon(1e-5));
  CHECK(r.run1(-1.0f, 64, out));  // still moving (36 frames left), then exactly the target
  for (uint32_t k = 36; k < 64; ++k) CHECK(out[k] == -1.0f);
  CHECK(out[35] == -1.0f);
  CHECK_FALSE(r.run1(-1.0f, 64, out));  // settled
  // +0 → −0 is a change of bits but no distance: a jump (nothing to ramp), never a NaN ramp.
  Ramp z(1, 100.0f);
  CHECK_FALSE(z.run1(0.0f, 8, out));
  CHECK_FALSE(z.run1(-0.0f, 8, out));
  CHECK(std::signbit(z.st().target[0]));
  // Non-finite values jump; a finite value after them jumps too (no ramp from infinity).
  CHECK_FALSE(z.run1(std::numeric_limits<float>::infinity(), 8, out));
  CHECK_FALSE(z.run1(2.0f, 8, out));
  CHECK(z.st().target[0] == 2.0f);
}

TEST_CASE("param_ramp: 8 lanes ramp independently; a lane reset starts at its value; hold writes the values") {
  Ramp r(8, 10.0f);
  std::vector<float> out;
  float x[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  CHECK_FALSE(r.run(x, 16, out));
  x[3] = 13.0f;  // one lane moves
  CHECK(r.run(x, 16, out));
  for (uint32_t k = 0; k < 16; ++k)
    for (uint32_t l = 0; l < 8; ++l) {
      const float want = l == 3 ? (k + 1 < 10 ? 3.0f + (k + 1) : 13.0f) : static_cast<float>(l);
      CHECK(out[k * 8 + l] == doctest::Approx(want).epsilon(1e-6));
    }
  // A voice's lane reset (pq/engine/voices.h: the PerLane words back to StateInit) starts that lane at its new value.
  const LaneStateInfo li = laneStateInfo(Atom::ParamRamp);
  REQUIRE(li.kind == LaneState::PerLane);
  CHECK(li.perLaneBytes == offsetof(ParamRampState, active));
  for (uint32_t off = 0; off < li.perLaneBytes; off += 32) reinterpret_cast<float*>(r.state.data())[off / 4 + 5] = 0.0f;
  x[5] = 50.0f;
  CHECK_FALSE(r.run(x, 16, out));
  CHECK(r.st().target[5] == 50.0f);
  std::vector<float> held(16 * 8, 0.0f);
  paramRampHold(r.state.data(), held.data(), 16, 8);
  for (uint32_t k = 0; k < 16; ++k)
    for (uint32_t l = 0; l < 8; ++l) CHECK(held[k * 8 + l] == x[l]);
}

TEST_CASE("param_ramp: any split of a block into calls gives the same bits (1 and 8 lanes)") {
  for (uint32_t lanes : {1u, 8u}) {
    Ramp a(lanes, 960.0f), b(lanes, 960.0f);
    std::vector<float> oa, ob, ca, cb;
    const float v0[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    const float v1[8] = {1.1f, -0.2f, 3.3f, 0.4f, -5.5f, 0.6f, 7.7f, 0.0f};
    a.run(v0, 64, oa);
    b.run(v0, 64, ob);
    for (int blk = 0; blk < 20; ++blk) {
      const float* v = blk < 3 ? v0 : v1;
      a.run(v, 64, oa);
      ca.insert(ca.end(), oa.begin(), oa.end());
      for (uint32_t n : {7u, 25u, 32u}) {  // 64 = 7 + 25 + 32
        if (b.run(v, n, ob)) cb.insert(cb.end(), ob.begin(), ob.end());
        else cb.insert(cb.end(), static_cast<size_t>(n) * lanes, -123.0f);
        v = blk < 3 ? v0 : v1;
      }
      if (!a.st().active) std::fill(ca.end() - static_cast<long>(64 * lanes), ca.end(), -123.0f);
    }
    REQUIRE(ca.size() == cb.size());
    // Where both wrote, the same bits (a call that rests after an earlier part of the block moved writes nothing).
    for (size_t i = 0; i < ca.size(); ++i)
      if (ca[i] != -123.0f && cb[i] != -123.0f) CHECK(std::memcmp(&ca[i], &cb[i], 4) == 0);
  }
}
