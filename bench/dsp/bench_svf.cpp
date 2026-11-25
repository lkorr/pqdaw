// EQ.1: the `svf` atom (pq/dsp/svf_design.h, kernels_svf.cpp) through findKernel as the interpreter and the JIT call it
// (both call the kernel), 64-frame blocks, white-noise input:
//   svf/<mode> s<N>              param-rate cutoff / q / gain (settled: no design in the timed calls), matched prewarp,
//                                N = 1, 8, 64 stages (64: the double cascade); rows L1 (ns/sample, a global filter) and
//                                L8 (ns/frame of 8 voices, one AVX2 vector per section and frame)
//   svf/lowpass s1 bilinear      the same with the bilinear prewarp (identical per-sample cost: only designs differ)
//   svf/<mode> s<N> audio cutoff the cutoff at audio rate, a curved sweep (an exponential per call: no two frames on a
//                                line, so every frame designs): matched / bilinear, 1 and 8 stages: a modulated filter
//   svf/<mode> s<N> ramp         EQ.1b: a knob drag's param ramp (the cutoff a triangle 1 → 2 kHz → 1 kHz, linear,
//                                960 frames = 20 ms each way, continuing across calls; 8 lanes: the same ramp in every
//                                lane, as a poly synth's filter knob): matched, 1 and 8 stages
#include <immintrin.h>

#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/svf_design.h"
#include "pq_bench.h"

namespace {

constexpr uint32_t kFrames = 64;
using pq::dsp::Atom;
constexpr const char* kModeNames[] = {"lowpass", "highpass", "bandpass", "notch", "bell", "lowshelf", "highshelf", "allpass"};

enum class Drive { Param, Audio, Ramp };

void benchSvf(pq::bench::Runner& runner, uint32_t mode, uint32_t stages, uint32_t prewarp, Drive drive) {
  const bool audioCutoff = drive != Drive::Param;
  std::string name = std::string("svf/") + kModeNames[mode] + " s" + std::to_string(stages);
  if (prewarp) name += " bilinear";
  if (drive == Drive::Audio) name += prewarp ? " audio cutoff" : " matched audio cutoff";
  if (drive == Drive::Ramp) name += " ramp";
  runner.add(name, [=](pq::bench::Context& ctx) {
    constexpr uint32_t kRampFrames = 960, kCycle = 2 * kRampFrames;  // 20 ms up, 20 ms down
    pq::AlignedBuffer<float> state(sizeof(pq::dsp::SvfState) / sizeof(float));
    pq::AlignedBuffer<float> in[7], out(kFrames * 8), ramp(kCycle * 8);
    for (auto& b : in) b = pq::AlignedBuffer<float>(kFrames * 8);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (uint32_t i = 0; i < kFrames * 8; ++i) in[0][i] = u(rng);
    for (uint32_t lanes : {1u, 8u}) {
      for (uint32_t l = 0; l < 8; ++l) {
        in[1][l] = 1000.0f;
        in[2][l] = 0.9f;
        in[3][l] = 2.0f;  // +6 dB (bell, shelves)
        in[4][l] = static_cast<float>(mode);
        in[5][l] = static_cast<float>(stages);
        in[6][l] = static_cast<float>(prewarp);
      }
      if (drive == Drive::Audio)  // a curved sweep per lane, a different cutoff every frame
        for (uint32_t t = 0; t < kFrames; ++t)
          for (uint32_t l = 0; l < lanes; ++l)
            in[1][t * lanes + l] = (500.0f + 7.0f * static_cast<float>(l)) * std::exp(0.02f * static_cast<float>(t));
      if (drive == Drive::Ramp)  // the param ramp's arithmetic (target − inc·rem), the same in every lane
        for (uint32_t t = 0; t < kCycle; ++t) {
          const bool up = t < kRampFrames;
          const float target = up ? 2000.0f : 1000.0f, inc = (up ? 1000.0f : -1000.0f) / kRampFrames;
          const float rem = static_cast<float>(kRampFrames - 1 - t % kRampFrames);
          for (uint32_t l = 0; l < lanes; ++l) ramp[t * lanes + l] = target - inc * rem;
        }
      pq::dsp::StateInit init;
      init.lanes = lanes;
      pq::dsp::initState(Atom::Svf, state.data(), init);
      const float* ins[7];
      for (uint32_t i = 0; i < 7; ++i) ins[i] = in[i].data();
      float* outs[1] = {out.data()};
      const uint32_t mask = audioCutoff ? 0x7Cu : 0x7Eu;  // x a block; cutoff a block or a pattern
      const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::Svf, lanes, mask);
      const pq::dsp::KernelArgs args{ins, outs, state.data(), nullptr, kFrames, 7, 1, mask};
      uint32_t pos = 0;  // Ramp: the frame of the cycle the next call starts at
      auto call = [&] {
        if (drive == Drive::Ramp) {
          ins[1] = ramp.data() + static_cast<size_t>(pos) * lanes;
          pos = (pos + kFrames) % kCycle;
        }
        fn(args);
      };
      call();  // the first design is outside the timed calls (param rate: settled from here on)
      ctx.measure([&] {
        call();
        pq::bench::doNotOptimize(out[0]);
      }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
    }
  });
}

struct ProtoSec {
  __m128 ic1, ic2, d1, b1, b2, m0, h1, h2, k, a, b;
};
__forceinline __m128 protoStep(ProtoSec& s, __m128 x) noexcept {
  const __m128 d = _mm_sub_ps(x, s.ic2);
  const __m128 n1 = _mm_fmadd_ps(s.b1, d, _mm_fmadd_ps(s.d1, s.ic1, s.ic1));
  const __m128 n2 = _mm_fmadd_ps(s.b2, d, _mm_fmadd_ps(s.b1, s.ic1, s.ic2));
  const __m128 y = _mm_fmadd_ps(s.h1, _mm_add_ps(s.ic1, n1), _mm_fmadd_ps(s.h2, _mm_add_ps(s.ic2, n2), _mm_mul_ps(s.m0, x)));
  s.ic1 = n1;
  s.ic2 = n2;
  return y;
}
__forceinline __m128 protoK(ProtoSec& s, __m128 x) noexcept {
  const __m128 y = _mm_fmadd_ps(s.k, x, _mm_fmadd_ps(s.a, s.ic1, _mm_mul_ps(s.b, s.ic2)));
  const __m128 d = _mm_sub_ps(x, s.ic2);
  const __m128 n1 = _mm_fmadd_ps(s.b1, d, _mm_fmadd_ps(s.d1, s.ic1, s.ic1));
  const __m128 n2 = _mm_fmadd_ps(s.b2, d, _mm_fmadd_ps(s.b1, s.ic1, s.ic2));
  s.ic1 = n1;
  s.ic2 = n2;
  return y;
}

// OP.7 prototype: an EQ's passes (4 bands × L, R; one section each, param rate) as the 8 kernel calls a schedule makes,
// against the same sections in one loop over frames (states in registers): scalar (8 recursions the core overlaps) and
// L / R as one 2-lane vector per band (4 recursions). `1-frame calls`: the kernel called frame by frame.
void benchEqProto(pq::bench::Runner& runner) {
  runner.add("svfproto/eq 4 bands stereo", [=](pq::bench::Context& ctx) {
    constexpr uint32_t kSecs = 8;
    const uint32_t modes[4] = {5, 4, 4, 6};
    const float hz[4] = {100.0f, 400.0f, 2500.0f, 8000.0f};
    std::vector<pq::AlignedBuffer<float>> states;
    pq::AlignedBuffer<float> x[2], buf[kSecs], pat[kSecs][6];
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (auto& b : x) {
      b = pq::AlignedBuffer<float>(kFrames);
      for (uint32_t i = 0; i < kFrames; ++i) b[i] = u(rng);
    }
    const uint32_t mask = 0x7Eu;
    const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::Svf, 1, mask);
    const float* ins[kSecs][7];
    float* outs[kSecs][1];
    std::vector<pq::dsp::KernelArgs> args, args1;
    for (uint32_t s = 0; s < kSecs; ++s) {
      const uint32_t band = s / 2, ch = s % 2;
      states.emplace_back(sizeof(pq::dsp::SvfState) / sizeof(float));
      pq::dsp::StateInit init;
      init.lanes = 1;
      init.svfSections = 1;
      pq::dsp::initState(Atom::Svf, states.back().data(), init);
      buf[s] = pq::AlignedBuffer<float>(kFrames);
      const float v[6] = {hz[band], 0.9f, 2.0f, static_cast<float>(modes[band]), 1.0f, 0.0f};
      for (uint32_t i = 0; i < 6; ++i) {
        pat[s][i] = pq::AlignedBuffer<float>(8);
        for (uint32_t l = 0; l < 8; ++l) pat[s][i][l] = v[i];
        ins[s][i + 1] = pat[s][i].data();
      }
      ins[s][0] = band == 0 ? x[ch].data() : buf[s - 2].data();
      outs[s][0] = buf[s].data();
    }
    for (uint32_t s = 0; s < kSecs; ++s) {
      args.push_back({ins[s], outs[s], states[s].data(), nullptr, kFrames, 7, 1, mask});
      fn(args.back());
    }
    ctx.measure([&] {
      for (uint32_t s = 0; s < kSecs; ++s) fn(args[s]);
      pq::bench::doNotOptimize(buf[7][0]);
    }, kFrames, "ns/frame", "8 kernel calls");
    // Frame by frame through the kernel (what a per-frame region's call costs).
    ctx.measure([&] {
      for (uint32_t t = 0; t < kFrames; ++t)
        for (uint32_t s = 0; s < kSecs; ++s) {
          const float* in1[7] = {ins[s][0] + t, ins[s][1], ins[s][2], ins[s][3], ins[s][4], ins[s][5], ins[s][6]};
          float* out1[1] = {outs[s][0] + t};
          fn(pq::dsp::KernelArgs{in1, out1, states[s].data(), nullptr, 1, 7, 1, mask});
        }
      pq::bench::doNotOptimize(buf[7][0]);
    }, kFrames, "ns/frame", "1-frame calls");
    // The sections of the states above, in one loop. Fields of a 1-lane section (kernels_svf.cpp Field).
    enum : uint32_t { kIc1 = 0, kIc2 = 2, kD1 = 4, kB1, kB2, kM0, kH1, kH2 };
    auto sec = [&](uint32_t s) { return static_cast<pq::dsp::SvfState*>(static_cast<void*>(states[s].data()))->sec; };
    ctx.measure([&] {
      __m128 ic1[kSecs], ic2[kSecs], c[kSecs][6];
      for (uint32_t s = 0; s < kSecs; ++s) {
        ic1[s] = _mm_load_ss(sec(s) + kIc1);
        ic2[s] = _mm_load_ss(sec(s) + kIc2);
        for (uint32_t f = 0; f < 6; ++f) c[s][f] = _mm_load_ss(sec(s) + kD1 + f);
      }
      const float* xl = x[0].data();
      const float* xr = x[1].data();
      float* ol = buf[6].data();
      float* orr = buf[7].data();
      for (uint32_t t = 0; t < kFrames; ++t) {
        __m128 v[2] = {_mm_load_ss(xl + t), _mm_load_ss(xr + t)};
        for (uint32_t s = 0; s < kSecs; ++s) {
          const __m128 xx = v[s % 2];
          const __m128 d = _mm_sub_ss(xx, ic2[s]);
          const __m128 n1 = _mm_fmadd_ss(c[s][1], d, _mm_fmadd_ss(c[s][0], ic1[s], ic1[s]));
          const __m128 n2 = _mm_fmadd_ss(c[s][2], d, _mm_fmadd_ss(c[s][1], ic1[s], ic2[s]));
          v[s % 2] = _mm_fmadd_ss(c[s][4], _mm_add_ss(ic1[s], n1),
                                  _mm_fmadd_ss(c[s][5], _mm_add_ss(ic2[s], n2), _mm_mul_ss(c[s][3], xx)));
          ic1[s] = n1;
          ic2[s] = n2;
        }
        _mm_store_ss(ol + t, v[0]);
        _mm_store_ss(orr + t, v[1]);
      }
      for (uint32_t s = 0; s < kSecs; ++s) {
        _mm_store_ss(sec(s) + kIc1, ic1[s]);
        _mm_store_ss(sec(s) + kIc2, ic2[s]);
      }
      pq::bench::doNotOptimize(buf[7][0]);
    }, kFrames, "ns/frame", "one loop scalar");
    ctx.measure([&] {
      __m128 ic1[4], ic2[4], c[4][6];
      for (uint32_t b = 0; b < 4; ++b) {
        ic1[b] = _mm_set_ps(0, 0, sec(2 * b + 1)[kIc1], sec(2 * b)[kIc1]);
        ic2[b] = _mm_set_ps(0, 0, sec(2 * b + 1)[kIc2], sec(2 * b)[kIc2]);
        for (uint32_t f = 0; f < 6; ++f) c[b][f] = _mm_set_ps(0, 0, sec(2 * b + 1)[kD1 + f], sec(2 * b)[kD1 + f]);
      }
      const float* xl = x[0].data();
      const float* xr = x[1].data();
      float* ol = buf[6].data();
      float* orr = buf[7].data();
      for (uint32_t t = 0; t < kFrames; ++t) {
        __m128 v = _mm_unpacklo_ps(_mm_load_ss(xl + t), _mm_load_ss(xr + t));
        for (uint32_t b = 0; b < 4; ++b) {
          const __m128 xx = v;
          const __m128 d = _mm_sub_ps(xx, ic2[b]);
          const __m128 n1 = _mm_fmadd_ps(c[b][1], d, _mm_fmadd_ps(c[b][0], ic1[b], ic1[b]));
          const __m128 n2 = _mm_fmadd_ps(c[b][2], d, _mm_fmadd_ps(c[b][1], ic1[b], ic2[b]));
          v = _mm_fmadd_ps(c[b][4], _mm_add_ps(ic1[b], n1), _mm_fmadd_ps(c[b][5], _mm_add_ps(ic2[b], n2), _mm_mul_ps(c[b][3], xx)));
          ic1[b] = n1;
          ic2[b] = n2;
        }
        _mm_store_ss(ol + t, v);
        _mm_store_ss(orr + t, _mm_shuffle_ps(v, v, 1));
      }
      for (uint32_t b = 0; b < 4; ++b) {
        _mm_store_ss(sec(2 * b) + kIc1, ic1[b]);
        _mm_store_ss(sec(2 * b + 1) + kIc1, _mm_shuffle_ps(ic1[b], ic1[b], 1));
        _mm_store_ss(sec(2 * b) + kIc2, ic2[b]);
        _mm_store_ss(sec(2 * b + 1) + kIc2, _mm_shuffle_ps(ic2[b], ic2[b], 1));
      }
      pq::bench::doNotOptimize(buf[7][0]);
    }, kFrames, "ns/frame", "one loop 2-lane");
    // The output as K·x + α·ic1 + β·ic2 (one multiply-add from x to y: the bands' forward path is 4 deep, not 16).
    auto derive = [&](uint32_t s, float* k) {
      const float* c = sec(s) + kD1;  // d1 b1 b2 m0 h1 h2
      const double d1 = c[0], b1 = c[1], b2 = c[2], m0 = c[3], h1 = c[4], h2 = c[5];
      k[0] = static_cast<float>(m0 + h1 * b1 + h2 * b2);
      k[1] = static_cast<float>(h1 * (2.0 + d1) + h2 * b1);
      k[2] = static_cast<float>(h2 * (2.0 - b2) - h1 * b1);
    };
    ctx.measure([&] {
      __m128 ic1[kSecs], ic2[kSecs], c[kSecs][6];
      for (uint32_t s = 0; s < kSecs; ++s) {
        float k[3];
        derive(s, k);
        ic1[s] = _mm_load_ss(sec(s) + kIc1);
        ic2[s] = _mm_load_ss(sec(s) + kIc2);
        for (uint32_t f = 0; f < 3; ++f) c[s][f] = _mm_load_ss(sec(s) + kD1 + f);
        for (uint32_t f = 0; f < 3; ++f) c[s][3 + f] = _mm_load_ss(k + f);
      }
      const float* xl = x[0].data();
      const float* xr = x[1].data();
      float* ol = buf[6].data();
      float* orr = buf[7].data();
      for (uint32_t t = 0; t < kFrames; ++t) {
        __m128 v[2] = {_mm_load_ss(xl + t), _mm_load_ss(xr + t)};
        for (uint32_t s = 0; s < kSecs; ++s) {
          const __m128 xx = v[s % 2];
          v[s % 2] = _mm_fmadd_ss(c[s][3], xx, _mm_fmadd_ss(c[s][4], ic1[s], _mm_mul_ss(c[s][5], ic2[s])));
          const __m128 d = _mm_sub_ss(xx, ic2[s]);
          const __m128 n1 = _mm_fmadd_ss(c[s][1], d, _mm_fmadd_ss(c[s][0], ic1[s], ic1[s]));
          const __m128 n2 = _mm_fmadd_ss(c[s][2], d, _mm_fmadd_ss(c[s][1], ic1[s], ic2[s]));
          ic1[s] = n1;
          ic2[s] = n2;
        }
        _mm_store_ss(ol + t, v[0]);
        _mm_store_ss(orr + t, v[1]);
      }
      for (uint32_t s = 0; s < kSecs; ++s) {
        _mm_store_ss(sec(s) + kIc1, ic1[s]);
        _mm_store_ss(sec(s) + kIc2, ic2[s]);
      }
      pq::bench::doNotOptimize(buf[7][0]);
    }, kFrames, "ns/frame", "one loop scalar, y = Kx + S");
    ctx.measure([&] {
      __m128 ic1[4], ic2[4], c[4][6];
      for (uint32_t b = 0; b < 4; ++b) {
        float k0[3], k1[3];
        derive(2 * b, k0);
        derive(2 * b + 1, k1);
        ic1[b] = _mm_set_ps(0, 0, sec(2 * b + 1)[kIc1], sec(2 * b)[kIc1]);
        ic2[b] = _mm_set_ps(0, 0, sec(2 * b + 1)[kIc2], sec(2 * b)[kIc2]);
        for (uint32_t f = 0; f < 3; ++f) c[b][f] = _mm_set_ps(0, 0, sec(2 * b + 1)[kD1 + f], sec(2 * b)[kD1 + f]);
        for (uint32_t f = 0; f < 3; ++f) c[b][3 + f] = _mm_set_ps(0, 0, k1[f], k0[f]);
      }
      const float* xl = x[0].data();
      const float* xr = x[1].data();
      float* ol = buf[6].data();
      float* orr = buf[7].data();
      for (uint32_t t = 0; t < kFrames; ++t) {
        __m128 v = _mm_unpacklo_ps(_mm_load_ss(xl + t), _mm_load_ss(xr + t));
        for (uint32_t b = 0; b < 4; ++b) {
          const __m128 xx = v;
          v = _mm_fmadd_ps(c[b][3], xx, _mm_fmadd_ps(c[b][4], ic1[b], _mm_mul_ps(c[b][5], ic2[b])));
          const __m128 d = _mm_sub_ps(xx, ic2[b]);
          const __m128 n1 = _mm_fmadd_ps(c[b][1], d, _mm_fmadd_ps(c[b][0], ic1[b], ic1[b]));
          const __m128 n2 = _mm_fmadd_ps(c[b][2], d, _mm_fmadd_ps(c[b][1], ic1[b], ic2[b]));
          ic1[b] = n1;
          ic2[b] = n2;
        }
        _mm_store_ss(ol + t, v);
        _mm_store_ss(orr + t, _mm_shuffle_ps(v, v, 1));
      }
      for (uint32_t b = 0; b < 4; ++b) {
        _mm_store_ss(sec(2 * b) + kIc1, ic1[b]);
        _mm_store_ss(sec(2 * b + 1) + kIc1, _mm_shuffle_ps(ic1[b], ic1[b], 1));
        _mm_store_ss(sec(2 * b) + kIc2, ic2[b]);
        _mm_store_ss(sec(2 * b + 1) + kIc2, _mm_shuffle_ps(ic2[b], ic2[b], 1));
      }
      pq::bench::doNotOptimize(buf[7][0]);
    }, kFrames, "ns/frame", "one loop 2-lane, y = Kx + S");
    // The same two forms with every state and coefficient a named local (registers, whatever the compiler does with
    // the arrays above).
    for (int form = 0; form < 4; ++form) {
      const bool kForm = (form & 1) != 0, two = (form & 2) != 0;
      ProtoSec q[kSecs];
      for (uint32_t i = 0; i < (two ? 4u : kSecs); ++i) {
        const uint32_t s0 = two ? 2 * i : i, s1 = two ? 2 * i + 1 : i;
        float k0[3], k1[3];
        derive(s0, k0);
        derive(s1, k1);
        auto pr = [&](uint32_t f) { return _mm_set_ps(0, 0, two ? sec(s1)[f] : 0.0f, sec(s0)[f]); };
        q[i].ic1 = pr(kIc1);
        q[i].ic2 = pr(kIc2);
        q[i].d1 = pr(kD1);
        q[i].b1 = pr(kB1);
        q[i].b2 = pr(kB2);
        q[i].m0 = pr(kM0);
        q[i].h1 = pr(kH1);
        q[i].h2 = pr(kH2);
        q[i].k = _mm_set_ps(0, 0, two ? k1[0] : 0.0f, k0[0]);
        q[i].a = _mm_set_ps(0, 0, two ? k1[1] : 0.0f, k0[1]);
        q[i].b = _mm_set_ps(0, 0, two ? k1[2] : 0.0f, k0[2]);
      }
      const float* xl = x[0].data();
      const float* xr = x[1].data();
      float* ol = buf[6].data();
      float* orr = buf[7].data();
      const char* names[4] = {"registers scalar", "registers scalar, y = Kx + S", "registers 2-lane", "registers 2-lane, y = Kx + S"};
      ctx.measure([&] {
        ProtoSec a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3], a4 = q[4], a5 = q[5], a6 = q[6], a7 = q[7];
        if (two) {
          for (uint32_t t = 0; t < kFrames; ++t) {
            __m128 v = _mm_unpacklo_ps(_mm_load_ss(xl + t), _mm_load_ss(xr + t));
            if (kForm) v = protoK(a3, protoK(a2, protoK(a1, protoK(a0, v))));
            else v = protoStep(a3, protoStep(a2, protoStep(a1, protoStep(a0, v))));
            _mm_store_ss(ol + t, v);
            _mm_store_ss(orr + t, _mm_shuffle_ps(v, v, 1));
          }
        } else {
          for (uint32_t t = 0; t < kFrames; ++t) {
            __m128 l = _mm_load_ss(xl + t), r = _mm_load_ss(xr + t);
            if (kForm) {
              l = protoK(a6, protoK(a4, protoK(a2, protoK(a0, l))));
              r = protoK(a7, protoK(a5, protoK(a3, protoK(a1, r))));
            } else {
              l = protoStep(a6, protoStep(a4, protoStep(a2, protoStep(a0, l))));
              r = protoStep(a7, protoStep(a5, protoStep(a3, protoStep(a1, r))));
            }
            _mm_store_ss(ol + t, l);
            _mm_store_ss(orr + t, r);
          }
        }
        q[0] = a0, q[1] = a1, q[2] = a2, q[3] = a3, q[4] = a4, q[5] = a5, q[6] = a6, q[7] = a7;
        pq::bench::doNotOptimize(buf[7][0]);
      }, kFrames, "ns/frame", names[form]);
    }
  });
}

}  // namespace

void addSvfBenches(pq::bench::Runner& runner) {
  benchEqProto(runner);
  for (uint32_t mode = 0; mode < pq::dsp::kSvfModeCount; ++mode)
    for (uint32_t stages : {1u, 8u, 64u}) {
      if ((mode == 3 || mode == 4) && stages > 1) continue;  // notch, bell: always one section
      benchSvf(runner, mode, stages, 0, Drive::Param);
    }
  benchSvf(runner, 0, 1, 1, Drive::Param);
  for (uint32_t prewarp : {0u, 1u}) {
    benchSvf(runner, 0, 1, prewarp, Drive::Audio);
    benchSvf(runner, 0, 8, prewarp, Drive::Audio);
    benchSvf(runner, 4, 1, prewarp, Drive::Audio);
    benchSvf(runner, 5, 1, prewarp, Drive::Audio);
  }
  for (uint32_t mode : {0u, 4u, 5u}) benchSvf(runner, mode, 1, 0, Drive::Ramp);
  benchSvf(runner, 0, 8, 0, Drive::Ramp);
  benchSvf(runner, 5, 8, 0, Drive::Ramp);
}
