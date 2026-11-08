// TMM.8: wavetable_live and wavetable_shape inside oversample regions through whole images — the interpreter and the
// JIT bit-identical (audio-rate and param-rate controls, corrections on, table swaps), nothing allocates in process().
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../dsp/frame_table_fixtures.h"
#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_host.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;
namespace dsp = pq::dsp;

namespace {

using TablePtr = std::shared_ptr<const dsp::FrameTable>;

jit::Compiler& compiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  return *c;
}

TablePtr build(std::vector<ftfix::Cycle> cycles, bool antiderivative) {
  dsp::FrameTableBuilder b;
  ftfix::SpecHolder s(std::move(cycles), 48000, 16);
  s.spec.antiderivative = antiderivative;
  auto t = b.build(s.spec);
  REQUIRE(t.ok());
  return std::make_shared<const dsp::FrameTable>(std::move(t.value()));
}

// Slot 1: Phase-mode tables; slot 2: Shape-mode tables (with the antiderivative arrays).
struct Rig {
  pq::EpochReclaimer rec{1};
  pq::EpochReclaimer::ReaderId reader{};
  TablePtr a1 = build({ftfix::basic(dsp::BasicCycle::Saw), ftfix::basic(dsp::BasicCycle::Square)}, false);
  TablePtr b1 = build(ftfix::drawnFixture(4), false);
  TablePtr a2 = build({ftfix::basic(dsp::BasicCycle::Square)}, true);
  TablePtr b2 = build({ftfix::basic(dsp::BasicCycle::Saw), ftfix::basic(dsp::BasicCycle::Triangle)}, true);
  std::unique_ptr<dsp::FrameTableSlot> s1 = std::make_unique<dsp::FrameTableSlot>(a1);
  std::unique_ptr<dsp::FrameTableSlot> s2 = std::make_unique<dsp::FrameTableSlot>(a2);
  std::vector<const dsp::FrameTableSlot*> slots{nullptr, s1.get(), s2.get()};
  eng::Resources res;
  Rig() {
    reader = *rec.registerReader();
    res.frameTables = slots;
  }
  ~Rig() { rec.unregisterReader(reader); }
  void swap(bool toB) {
    s1->publish(toB ? b1 : a1, rec);
    s2->publish(toB ? b2 : a2, rec);
    rec.collect();
  }
};

pq::Result<std::unique_ptr<eng::ProgramImage>> buildImage(int backend, const Schedule& s, const eng::Resources& res) {
  if (backend == 0) {
    eng::InterpretOptions io;
    io.maxBlockFrames = 64;
    io.maxVoiceGroups = 1;
    io.resources = &res;
    return eng::buildInterpreted(s, io);
  }
  jit::CompileOptions o;
  o.maxBlockFrames = 64;
  o.maxVoiceGroups = 1;
  o.resources = &res;
  return compiler().compile(s, o);
}

// The Wavetable's live path (modulated warp and sync from an audio input, a param fold, Pulse width, corrections) at
// ×4 and Shape mode at ×2, as the meta expansion builds them.
const char* kPatch = R"(
patch W {
  in ph : audio
  in m : audio
  param fold = 0.2 [0..1]
  param pos = 0.3 [0..1]
  out y : audio
  out z : audio
  oversample(4, phase: ph) {
    y = wavetable_live(ph, pos, m, fold, m * 0.5 + 0.5, 1, 2, 1)
  }
  oversample(2) {
    z = wavetable_shape(m * 1.5, pos, 2)
  }
}
)";

}  // namespace

TEST_CASE("wavetable_live / wavetable_shape in oversample regions: interpreter == JIT through swaps; no allocation") {
  const Compiled c = compileText(kPatch);
  constexpr uint32_t kBlock = 41, kN = 41 * 100;
  std::vector<float> phase(kN), mod(kN);
  double acc = 0.0;
  for (uint32_t n = 0; n < kN; ++n) {
    phase[n] = static_cast<float>(acc);
    acc += (220.0 + 1500.0 * n / kN) / 48000.0;
    acc -= std::floor(acc);
    mod[n] = static_cast<float>(std::sin(n * 2.0 * 3.14159265358979 * 700.0 / 48000.0));
  }
  std::vector<float> outs[2][2];
  uint64_t violations = 0;
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    Rig rig;
    auto img = buildImage(backend, c.sched, rig.res);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    const eng::ImageInterface& face = (*img)->io();
    REQUIRE(face.globalIn.size() == 2);
    REQUIRE(face.globalOut.size() == 2);
    std::vector<float> in[2] = {std::vector<float>(64), std::vector<float>(64)};
    std::vector<float> out[2] = {std::vector<float>(64), std::vector<float>(64)};
    const float* inP[2] = {in[0].data(), in[1].data()};
    float* outP[2] = {out[0].data(), out[1].data()};
    for (uint32_t t0 = 0, blk = 0; t0 < kN; t0 += kBlock, ++blk) {
      if (blk == 30) rig.swap(true);
      if (blk == 31) rig.swap(false);  // inside the first fade: it waits (AU.fold), then fades on
      if (blk == 70) rig.swap(true);
      for (uint32_t s = 0; s < 2; ++s)
        std::copy_n((face.globalIn[s].ordinal == 0 ? phase : mod).begin() + t0, kBlock, in[s].begin());
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = 48000.0;
      ctx.sampleTime = t0;
      ctx.in = inP;
      ctx.numIn = 2;
      ctx.out = outP;
      ctx.numOut = 2;
      violations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        (*img)->process(ctx);
      });
      rig.rec.quiescent(rig.reader);
      const pq::graph::Patch& P = c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch;
      for (uint32_t s = 0; s < 2; ++s) {
        const uint32_t k = P.nodeName(face.globalOut[s].node) == "z" ? 1u : 0u;
        outs[backend][k].insert(outs[backend][k].end(), out[s].begin(), out[s].begin() + kBlock);
      }
    }
  }
  CHECK(violations == 0);
  for (int k = 0; k < 2; ++k) {
    CAPTURE(k);
    REQUIRE(outs[0][k].size() == outs[1][k].size());
    CHECK(std::memcmp(outs[0][k].data(), outs[1][k].data(), outs[0][k].size() * sizeof(float)) == 0);
    double peak = 0;
    bool finite = true;
    for (const float v : outs[0][k]) {
      finite &= std::isfinite(v);
      peak = std::max(peak, std::fabs(static_cast<double>(v)));
    }
    CHECK(finite);
    CHECK(peak > 0.2);
  }
}
