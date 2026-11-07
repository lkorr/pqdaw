// GU.1: the `curve` atom (a driven control's curve, pq/dsp/curve.h) per voice at audio rate: 8 voices (L8, one AVX2
// voice group), the position a different ramp per voice, 64-frame blocks, through findKernel as the interpreter and the
// JIT call it (both call the kernel). ns per frame of 8 voices (divide by 8 for one voice), and the 1-lane rows (ns per
// sample of a global control):
//   curve/linear 1 seg      one straight segment (a linear knob: no search, no transcendental)
//   curve/exp 1 seg         one Exp segment (an octave / exp0 knob: one expm1)
//   curve/exp 16 seg        16 Exp segments (the search: 15 compares, 5 gathers, one expm1)
//   curve/mixed 16 seg      16 segments of all five shapes (every shape computed, blended per lane)
//   curve/dB 1 seg          a dB gain: one straight segment and the dB → gain conversion
#include <cmath>
#include <memory>
#include <string>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/curve.h"
#include "pq_bench.h"

namespace {

constexpr uint32_t kFrames = 64;
using pq::dsp::Atom;

pq::dsp::Curve segments16(bool mixed) {
  pq::dsp::Curve c = pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, 20.0, 20000.0);
  c.preset = pq::dsp::CurvePreset::Custom;
  c.segments = 16;
  for (uint32_t i = 0; i <= 16; ++i) c.points[i] = {static_cast<double>(i) / 16.0, static_cast<double>(i) / 16.0};
  for (uint32_t i = 0; i < 16; ++i)
    c.shapes[i] = {mixed ? static_cast<pq::dsp::CurveShape>(i % pq::dsp::kCurveShapeCount) : pq::dsp::CurveShape::Exp, 2.0};
  return c;
}

void benchCurve(pq::bench::Runner& runner, const std::string& name, const pq::dsp::Curve& curve) {
  runner.add("curve/" + name, [curve](pq::bench::Context& ctx) {
    auto slot = std::make_unique<pq::dsp::CurveSlot>(pq::dsp::compileCurve(curve));
    pq::AlignedBuffer<float> x(kFrames * 8), out(kFrames * 8), convert(8);
    for (uint32_t i = 0; i < 8; ++i) convert[i] = 1.0f;
    for (uint32_t lanes : {1u, 8u}) {
      for (uint32_t t = 0; t < kFrames; ++t)
        for (uint32_t l = 0; l < lanes; ++l)  // a ramp per voice over the whole knob, voices 1/9 apart
          x[t * lanes + l] = std::fmod(static_cast<float>(t) / kFrames + static_cast<float>(l) / 9.0f, 1.0f);
      const float* ins[2] = {x.data(), convert.data()};
      float* outs[1] = {out.data()};
      const uint32_t mask = 0b10;  // x a block, convert a pattern
      const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::Curve, lanes, mask);
      const pq::dsp::KernelArgs args{ins, outs, nullptr, slot.get(), kFrames, 2, 1, mask};
      ctx.measure([&] {
        fn(args);
        pq::bench::doNotOptimize(out[0]);
      }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
    }
  });
}

}  // namespace

void addCurveBenches(pq::bench::Runner& runner) {
  benchCurve(runner, "linear 1 seg", pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, 20.0, 20000.0));
  benchCurve(runner, "exp 1 seg", pq::dsp::curvePreset(pq::dsp::CurvePreset::Octave, 20.0, 20000.0));
  benchCurve(runner, "exp 16 seg", segments16(false));
  benchCurve(runner, "mixed 16 seg", segments16(true));
  pq::dsp::Curve db = pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, -70.0, 6.0);
  db.output = pq::dsp::CurveOutput::DbToGain;
  benchCurve(runner, "dB 1 seg", db);
}
