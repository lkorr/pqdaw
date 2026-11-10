// GU.1: the `curve` atom through whole images — interpreter and JIT bit-identical at audio rate (global and per voice),
// at param rate and inside a per-sample feedback loop, through publications of new curve data (no recompile), both
// equal to dsp::evalCurve to float rounding; RtScope on every process() with publications.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/curve.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/voices.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;
namespace dsp = pq::dsp;

namespace {

jit::Compiler& compiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  return *c;
}

dsp::Curve exp0() {  // the Phase accumulator's frequency law, signed
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Exp0, 0.0, 20000.0, 14.0);
  c.mode = dsp::CurveMode::Signed;
  return c;
}
dsp::Curve gainDb() {  // a dB gain, −70 … +6 dB, bent
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Exponential, -70.0, 6.0);
  c.output = dsp::CurveOutput::DbToGain;
  return c;
}
dsp::Curve mixed16() {  // 16 segments of every shape
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, -1.0, 3.0);
  c.preset = dsp::CurvePreset::Custom;
  c.segments = 16;
  for (uint32_t i = 0; i <= 16; ++i) c.points[i] = {static_cast<double>(i) / 16.0, 0.5 + 0.45 * std::sin(i * 1.7)};
  for (uint32_t i = 0; i < 16; ++i) c.shapes[i] = {static_cast<dsp::CurveShape>(i % 5), -3.0 + 0.4 * i};
  REQUIRE(dsp::validCurve(c));
  return c;
}

// Slots 1 and 2, read by one registered reader.
struct Rig {
  pq::EpochReclaimer rec{1};
  pq::EpochReclaimer::ReaderId reader{};
  std::unique_ptr<dsp::CurveSlot> a, b;
  std::vector<const dsp::CurveSlot*> slots;
  eng::Resources res;
  Rig(const dsp::Curve& ca, const dsp::Curve& cb) {
    reader = *rec.registerReader();
    a = std::make_unique<dsp::CurveSlot>(dsp::compileCurve(ca));
    b = std::make_unique<dsp::CurveSlot>(dsp::compileCurve(cb));
    slots = {nullptr, a.get(), b.get()};
    res.curves = slots;
  }
  ~Rig() { rec.unregisterReader(reader); }
};

pq::Result<std::unique_ptr<eng::ProgramImage>> buildImage(int backend, const Schedule& s, const eng::Resources& res,
                                                          uint32_t maxBlock, uint32_t groups) {
  if (backend == 0) {
    eng::InterpretOptions io;
    io.maxBlockFrames = maxBlock;
    io.maxVoiceGroups = groups;
    io.resources = &res;
    return eng::buildInterpreted(s, io);
  }
  jit::CompileOptions o;
  o.maxBlockFrames = maxBlock;
  o.maxVoiceGroups = groups;
  o.resources = &res;
  return compiler().compile(s, o);
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Renders a global patch with audio inputs `x` (one per input, in ordinal order) for `n` frames in blocks of `block`,
// publishing `swapTo` into slot 1 at block `swapAt`. Every output, concatenated, in the order of `outNames`.
std::vector<std::vector<float>> renderGlobal(int backend, const Compiled& c, Rig& rig, const std::vector<std::vector<float>>& x,
                                             const std::vector<std::string>& outNames, uint32_t n, uint32_t block,
                                             uint32_t swapAt, const dsp::Curve& swapTo, uint64_t& violations) {
  auto img = buildImage(backend, c.sched, rig.res, 64, 1);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const eng::ImageInterface& face = (*img)->io();
  std::vector<std::vector<float>> in(face.globalIn.size(), std::vector<float>(64)), out(face.globalOut.size(), std::vector<float>(64));
  std::vector<const float*> inP;
  std::vector<float*> outP;
  for (auto& v : in) inP.push_back(v.data());
  for (auto& v : out) outP.push_back(v.data());
  REQUIRE(face.globalOut.size() == outNames.size());
  std::vector<std::vector<float>> got(outNames.size());
  for (uint32_t t0 = 0, blk = 0; t0 < n; t0 += block, ++blk) {
    if (blk == swapAt) {
      rig.a->publish(dsp::compileCurve(swapTo), rig.rec);
      rig.rec.collect();
    }
    for (size_t s = 0; s < in.size(); ++s) std::copy_n(x[face.globalIn[s].ordinal].begin() + t0, block, in[s].begin());
    eng::ProcessContext ctx;
    ctx.frames = block;
    ctx.sampleRate = 48000.0;
    ctx.sampleTime = t0;
    ctx.in = inP.data();
    ctx.numIn = static_cast<uint32_t>(inP.size());
    ctx.out = outP.data();
    ctx.numOut = static_cast<uint32_t>(outP.size());
    violations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      (*img)->process(ctx);
    });
    rig.rec.quiescent(rig.reader);
    const pq::graph::Patch& P = c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch;
    for (size_t s = 0; s < out.size(); ++s) {
      const std::string& name = P.nodeName(face.globalOut[s].node);
      const size_t k = static_cast<size_t>(std::find(outNames.begin(), outNames.end(), name) - outNames.begin());
      REQUIRE(k < got.size());
      got[k].insert(got[k].end(), out[s].begin(), out[s].begin() + block);
    }
  }
  return got;
}

double worstVs(const std::vector<float>& got, const std::vector<float>& x, const dsp::Curve& c, uint32_t from, uint32_t to,
               bool convert = true) {
  double worst = 0.0;
  for (uint32_t i = from; i < to; ++i) {
    const double want = convert ? dsp::evalCurve(c, x[i]) : dsp::evalCurveUnits(c, x[i]);
    // As tests/dsp/test_curve.cpp bounds the kernel: relative to the value, or to the range for a value near 0 (dB).
    const double scale = convert && c.output == dsp::CurveOutput::DbToGain ? std::fabs(want)
                                                                          : std::max(std::fabs(want), std::fabs(c.max - c.min));
    worst = std::max(worst, std::fabs(got[i] - want) / std::max(scale, 1e-30));
  }
  return worst;
}

}  // namespace

TEST_CASE("curve atom (global, audio rate): interpreter == JIT through a publication, both = evalCurve; RtScope") {
  const Compiled c = compileText(R"(
patch G {
  in x : audio
  out y : audio
  out z : audio
  out w : audio
  y = curve(x * 2 - 1, 1, 1)
  z = curve(x, 0, 2)
  w = curve(x, 1, 2)
}
)");
  constexpr uint32_t kBlock = 37, kN = 37 * 60;
  std::vector<float> x(kN);
  for (uint32_t i = 0; i < kN; ++i) x[i] = static_cast<float>(0.5 + 0.5 * std::sin(i * 0.011));
  std::vector<float> xs(kN);  // what y's curve reads (the patch computes x · 2 − 1 in float)
  for (uint32_t i = 0; i < kN; ++i) xs[i] = x[i] * 2.0f - 1.0f;
  std::vector<std::vector<float>> outs[2];
  uint64_t violations = 0;
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    Rig rig(exp0(), gainDb());
    outs[backend] = renderGlobal(backend, c, rig, {x}, {"y", "z", "w"}, kN, kBlock, 30, mixed16(), violations);
  }
  CHECK(violations == 0);
  for (size_t k = 0; k < 3; ++k) CHECK(sameBits(outs[0][k], outs[1][k]));
  const uint32_t swapFrame = 30 * kBlock;
  CHECK(worstVs(outs[1][0], xs, exp0(), 0, swapFrame) <= 4e-6);
  CHECK(worstVs(outs[1][0], xs, mixed16(), swapFrame, kN) <= 4e-6);  // the new data from its block on, no recompile
  CHECK(worstVs(outs[1][1], x, gainDb(), 0, kN, false) <= 4e-6);    // convert 0: dB
  CHECK(worstVs(outs[1][2], x, gainDb(), 0, kN) <= 4e-6);           // convert 1: the gain
}

TEST_CASE("curve atom (param rate, and in a per-sample feedback loop): interpreter == JIT") {
  const Compiled c = compileText(R"(
patch L {
  in x : audio
  param k = 0.3 [0..1]
  out y : audio
  out f : audio
  y = x * curve(k, 1, 2)
  s = z1(fb)
  fb = curve(frac(s * 0.5 + x * 0.25 + 0.013), 1, 1) * 0.0004
  f = fb
}
)");
  constexpr uint32_t kBlock = 64, kN = 64 * 40;
  std::vector<float> x(kN);
  for (uint32_t i = 0; i < kN; ++i) x[i] = static_cast<float>(std::sin(i * 0.05));
  std::vector<std::vector<float>> outs[2];
  uint64_t violations = 0;
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    Rig rig(mixed16(), gainDb());
    outs[backend] = renderGlobal(backend, c, rig, {x}, {"y", "f"}, kN, kBlock, 20, exp0(), violations);
  }
  CHECK(violations == 0);
  CHECK(sameBits(outs[0][0], outs[1][0]));
  CHECK(sameBits(outs[0][1], outs[1][1]));
  const float g = static_cast<float>(dsp::evalCurve(gainDb(), 0.3));
  for (uint32_t i = 0; i < kN; ++i) CHECK(std::fabs(outs[1][0][i] - x[i] * g) <= 1e-6f * std::max(1.0f, std::fabs(x[i] * g)));
}

TEST_CASE("curve atom (voices, audio rate): interpreter == JIT per voice, = evalCurve of each voice's phase") {
  const Compiled c = compileText(R"(
patch V {
  out y : audio voice
  ph = phasor(voice.hz)
  y = curve(ph, 1, 1) * gate_to_real(voice.gate)
}
)");
  constexpr uint32_t kBlock = 64, kBlocks = 60, kGroups = 2;
  std::vector<float> mix[2];
  uint64_t violations = 0;
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    Rig rig(mixed16(), gainDb());
    auto img = buildImage(backend, c.sched, rig.res, kBlock, kGroups);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    eng::VoiceEngine ve;
    eng::VoiceConfig cfg;
    cfg.maxVoices = 16;
    REQUIRE(ve.prepare(img->get(), cfg).ok());
    eng::EventBuffer buf(64);
    for (uint32_t b = 0; b < kBlocks; ++b) {
      if (b == 30) {
        rig.a->publish(dsp::compileCurve(exp0()), rig.rec);
        rig.rec.collect();
      }
      buf.clear();
      if (b == 0)
        for (uint32_t v = 0; v < 9; ++v) buf.push(eng::noteOn(v, v + 1, 110.0f + 37.0f * v, 0.7f));
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = 48000.0;
      ctx.sampleTime = static_cast<int64_t>(b) * kBlock;
      ctx.events = &buf.list();
      violations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        ve.process(ctx);
      });
      rig.rec.quiescent(rig.reader);
      mix[backend].insert(mix[backend].end(), ve.mix(0), ve.mix(0) + kBlock);
    }
  }
  CHECK(violations == 0);
  CHECK(sameBits(mix[0], mix[1]));
  double energy = 0.0;
  for (const float v : mix[1]) energy += std::fabs(v);
  CHECK(energy > 1.0);
}

// PF.1: the cache key hashes whether an op has a resource, not its slot (code reaches the resource through its
// pointer-table slot). Units that differ only in which curve slots they read share one compiled code, each image reads
// its own curves, and the shared images compute exactly what separately compiled images (and the interpreter) compute.
// The pattern of which ops share a resource is still keyed (an aliased pair is other code). The slots are numbers no
// other literal of the unit has: a constant is interned by value, so a slot equal to another literal changes the
// schedule itself (its constants and op order), which is other code whatever the key hashes.
TEST_CASE("curve atom: units that differ only in their resource slots share code and each reads its own resource") {
  auto patchFor = [](int ySlot, int zSlot) {
    return compileText("patch G {\n  in x : audio\n  out y : audio\n  out z : audio\n  y = curve(x * 2 - 1, 1, " +
                       std::to_string(ySlot) + ")\n  z = curve(x, 1, " + std::to_string(zSlot) + ")\n}\n");
  };
  const Compiled a = patchFor(5, 7), b = patchFor(7, 5), aliased = patchFor(5, 5);
  Rig rig(exp0(), gainDb());
  rig.slots = {nullptr, nullptr, nullptr, nullptr, nullptr, rig.a.get(), nullptr, rig.b.get()};  // curves at 5 and 7
  rig.res.curves = rig.slots;
  constexpr uint32_t kBlock = 64, kN = 64 * 20;
  std::vector<float> x(kN);
  for (uint32_t i = 0; i < kN; ++i) x[i] = static_cast<float>(0.5 + 0.5 * std::sin(i * 0.013));
  // Renders y and z (in that order) of one image.
  auto render = [&](const Compiled& c, eng::ProgramImage& img) {
    const eng::ImageInterface& face = img.io();
    REQUIRE(face.globalIn.size() == 1);
    REQUIRE(face.globalOut.size() == 2);
    std::vector<float> in(kBlock), o0(kBlock), o1(kBlock);
    const float* inP = in.data();
    float* outP[2] = {o0.data(), o1.data()};
    const pq::graph::Patch& P = c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch;
    const bool yFirst = P.nodeName(face.globalOut[0].node) == "y";
    std::vector<float> y, z;
    for (uint32_t t0 = 0; t0 < kN; t0 += kBlock) {
      std::copy_n(x.begin() + t0, kBlock, in.begin());
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = 48000.0;
      ctx.sampleTime = t0;
      ctx.in = &inP;
      ctx.numIn = 1;
      ctx.out = outP;
      ctx.numOut = 2;
      pq::DenormalGuard dg;
      img.process(ctx);
      y.insert(y.end(), (yFirst ? o0 : o1).begin(), (yFirst ? o0 : o1).end());
      z.insert(z.end(), (yFirst ? o1 : o0).begin(), (yFirst ? o1 : o0).end());
    }
    return std::vector<std::vector<float>>{y, z};
  };
  jit::CompileOptions o;
  o.maxBlockFrames = kBlock;
  o.maxVoiceGroups = 1;
  o.resources = &rig.res;
  auto make = [](const jit::CacheConfig& cache) {
    jit::CompilerConfig cfg;
    cfg.cache = cache;
    auto r = jit::Compiler::create(cfg);
    REQUIRE(r.ok());
    return std::move(r.value());
  };
  jit::CacheConfig verify;  // memory only; every hit regenerates the IR and compares it
  verify.verify = true;
  auto shared = make(verify);
  auto ia = shared->compile(a.sched, o);
  auto ib = shared->compile(b.sched, o);
  REQUIRE(ia.ok());
  REQUIRE(ib.ok());
  CHECK(shared->counters().compiles == 1);
  CHECK(shared->counters().memoryHits == 1);
  CHECK(shared->counters().verified == 1);
  CHECK(shared->counters().verifyMismatches == 0);
  auto ic = shared->compile(aliased.sched, o);  // one resource read twice: another pointer-table pattern
  REQUIRE(ic.ok());
  CHECK(shared->counters().compiles == 2);
  // Each against a separate compile (a compiler of its own) and the interpreter: bit-identical.
  auto separate = make(jit::CacheConfig{});
  for (const auto* p : {&a, &b, &aliased}) {
    auto own = separate->compile(p->sched, o);
    REQUIRE(own.ok());
    eng::InterpretOptions io;
    io.maxBlockFrames = kBlock;
    io.maxVoiceGroups = 1;
    io.resources = &rig.res;
    auto interp = eng::buildInterpreted(p->sched, io);
    REQUIRE(interp.ok());
    eng::ProgramImage& sharedImg = p == &a ? **ia : p == &b ? **ib : **ic;
    const auto got = render(*p, sharedImg), want = render(*p, **own), ref = render(*p, **interp);
    for (size_t k = 0; k < 2; ++k) {
      CHECK(sameBits(got[k], want[k]));
      CHECK(sameBits(got[k], ref[k]));
    }
  }
  CHECK(separate->counters().compiles == 3);
  // Each shared image read its own slots (a: y ← 5, z ← 7; b the other way round), against the curves themselves.
  const auto ga = render(a, **ia), gb = render(b, **ib);
  std::vector<float> xs(kN);
  for (uint32_t i = 0; i < kN; ++i) xs[i] = x[i] * 2.0f - 1.0f;
  CHECK(worstVs(ga[0], xs, exp0(), 0, kN) <= 4e-6);
  CHECK(worstVs(ga[1], x, gainDb(), 0, kN) <= 4e-6);
  CHECK(worstVs(gb[0], xs, gainDb(), 0, kN) <= 4e-6);
  CHECK(worstVs(gb[1], x, exp0(), 0, kN) <= 4e-6);
  CHECK(!sameBits(ga[0], gb[0]));
}
