// T1b.1: the LLVM JIT backend. Every T1.7 interpreter test, run through the JIT: the whole corpus (every patch of
// tests/graph/cases and tests/engine/patches × 2 param sets × blocks 1/37/64/1024) against the interpreter (≤ 1e-6)
// and the reference evaluator (≤ 1e-5); bit-identity with the interpreter when contraction is off; every
// optimisation level; the ProgramImage contract cases (params, rejected blocks, missing inputs, interface, build
// errors, state arena, ImageRunner, NullDevice); RtScope around every process(); compile time for 100/1000 nodes.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/null_device.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;

namespace {

constexpr double kTolInterp = 1e-6;  // JIT vs interpreter (TASKS T1b.1)
constexpr double kTolRef = 1e-5;     // JIT vs reference evaluator (the interpreter's own criterion)
constexpr uint32_t kFrames = 2048;
constexpr uint32_t kMaxBlock = 1024;
constexpr uint32_t kBlocks[] = {1, 37, 64, kMaxBlock};

jit::Compiler& compiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    if (!r) {
      std::fprintf(stderr, "%s\n", r.error().message.c_str());
      std::abort();
    }
    return std::move(r.value());
  }();
  return *c;
}

ImageBuilder jitBuilder(jit::CompileOptions base = {}) {
  return [base](const Schedule& s, const eng::InterpretOptions& io) {
    jit::CompileOptions o = base;
    o.maxBlockFrames = io.maxBlockFrames;
    o.maxVoiceGroups = io.maxVoiceGroups;
    o.resources = io.resources;
    return compiler().compile(s, o);
  };
}

pq::Result<std::unique_ptr<eng::ProgramImage>> compileJit(const Schedule& s, jit::CompileOptions o = {}) {
  return compiler().compile(s, o);
}

uint32_t voicesFor(const Compiled& c) {
  for (const Stage& s : c.sched.stages)
    if (s.poly == Poly::Voice) return 11;  // two groups, the second one partly active
  return 0;
}

fs::path jitPatchesDir() { return fs::path(PQ_TEST_DATA_DIR) / "jit" / "patches"; }

// The T1.7 corpus plus tests/jit/patches (stateful atoms inside per-sample loops: the inline per-frame code).
std::vector<fs::path> allPatchFiles() {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(patchesDir())) files.push_back(p);
  for (const fs::path& p : text_util::pqpFiles(jitPatchesDir())) files.push_back(p);
  return files;
}

// Samples that differ in any bit (NaN payloads aside: both NaN counts as equal).
uint64_t bitDifferences(const Streams& a, const Streams& b) {
  uint64_t n = 0;
  for (const auto& [key, x] : a) {
    const auto it = b.find(key);
    if (it == b.end() || it->second.size() != x.size()) return UINT64_MAX;
    for (size_t t = 0; t < x.size(); ++t) {
      const double p = x[t], q = it->second[t];
      if (std::isnan(p) && std::isnan(q)) continue;
      if (std::memcmp(&p, &q, sizeof p) != 0) ++n;
    }
  }
  return n;
}

struct CorpusStats {
  uint32_t patches = 0, files = 0, voicePatches = 0, configs = 0;
  double maxVsInterp = 0, maxVsRef = 0;
  std::string worstInterp, worstRef;
};

// One patch × voices × param set × every block size: JIT vs interpreter and vs the reference.
void checkPatch(const Compiled& c, uint32_t voices, int paramSet, const jit::CompileOptions& jo, CorpusStats& cs,
                bool requireBitIdentical, const uint32_t* blocks, size_t numBlocks) {
  const Setup st(c, voices, paramSet);
  std::string why;
  const Streams ref = runReference(st, kFrames, why);
  REQUIRE_MESSAGE(why.empty(), c.file << ":" << c.name << ": reference evaluator: " << why);
  eng::InterpretOptions io;
  io.maxBlockFrames = kMaxBlock;
  io.maxVoiceGroups = std::max<uint32_t>(1, (voices + 7) / 8);
  auto face = eng::buildInterpreted(c.sched, io);
  REQUIRE(face.ok());
  for (size_t bi = 0; bi < numBlocks; ++bi) {
    const uint32_t block = blocks[bi];
    INFO(c.file << ":" << c.name << " voices " << voices << " params " << paramSet << " block " << block);
    const InterpRun interp = runInterp(st, kFrames, block, kMaxBlock);
    REQUIRE_MESSAGE(interp.error.empty(), interp.error);
    const InterpRun run = runInterp(st, kFrames, block, kMaxBlock, jitBuilder(jo));
    REQUIRE_MESSAGE(run.error.empty(), run.error);
    CHECK(run.rtViolations == 0);
    const Mismatch mi = compare(c, interp.streams, run.streams, (*face)->io());
    const Mismatch mr = compare(c, ref, run.streams, (*face)->io());
    CHECK_MESSAGE(mi.maxErr <= kTolInterp, "vs interpreter: max error " << mi.maxErr << ": " << mi.where);
    CHECK_MESSAGE(mr.maxErr <= kTolRef, "vs reference: max error " << mr.maxErr << ": " << mr.where);
    if (requireBitIdentical) CHECK_MESSAGE(bitDifferences(interp.streams, run.streams) == 0, "not bit-identical: " << mi.where);
    if (mi.maxErr >= cs.maxVsInterp) {
      cs.maxVsInterp = mi.maxErr;
      cs.worstInterp = c.file + ":" + c.name + " " + mi.where + " (block " + std::to_string(block) + ")";
    }
    if (mr.maxErr >= cs.maxVsRef) {
      cs.maxVsRef = mr.maxErr;
      cs.worstRef = c.file + ":" + c.name + " " + mr.where + " (block " + std::to_string(block) + ")";
    }
    ++cs.configs;
  }
}

CorpusStats runCorpus(const jit::CompileOptions& jo, bool requireBitIdentical, const uint32_t* blocks, size_t numBlocks) {
  CorpusStats cs;
  std::vector<std::string> skipped;
  for (const fs::path& path : allPatchFiles()) {
    ++cs.files;
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      const uint32_t voices = voicesFor(c);
      cs.voicePatches += voices > 0;
      for (int paramSet = 0; paramSet < 2; ++paramSet) checkPatch(c, voices, paramSet, jo, cs, requireBitIdentical, blocks, numBlocks);
      ++cs.patches;
    }
  }
  return cs;
}

// A chain of n nodes mixing what real patches have: element-wise math (exact and fast), param-rate math, a z1
// outside any loop, and a one-pole feedback loop every 10 nodes.
std::string mixedChain(int n) {
  std::string t = "patch Chain {\n  in x : audio\n  param p = 0.5 [0..1]\n  out y : audio\n  a0 = x\n";
  int i = 1;
  while (i <= n) {
    const std::string prev = "a" + std::to_string(i - 1), cur = "a" + std::to_string(i);
    switch (i % 10) {
      case 0: t += "  " + cur + " = z1(" + cur + " + (" + prev + " - " + cur + ") * 0.1)\n"; break;  // 3 nodes
      case 1: t += "  " + cur + " = " + prev + " * 0.99 + 0.01\n"; break;
      case 2: t += "  " + cur + " = tanh(" + prev + ")\n"; break;
      case 3: t += "  " + cur + " = min(" + prev + ", 0.9)\n"; break;
      case 4: t += "  " + cur + " = fast_tanh(" + prev + " * p)\n"; break;
      case 5: t += "  " + cur + " = " + prev + " + z1(" + prev + ")\n"; break;
      case 6: t += "  " + cur + " = max(" + prev + ", -0.9)\n"; break;
      case 7: t += "  " + cur + " = abs(" + prev + ") * 0.5\n"; break;
      case 8: t += "  " + cur + " = " + prev + " + 0.25\n"; break;
      default: t += "  " + cur + " = fast_sin(" + prev + ")\n"; break;
    }
    ++i;
  }
  return t + "  y = a" + std::to_string(n) + "\n}\n";
}

}  // namespace

TEST_CASE("jit: host CPU and features") {
  MESSAGE("JIT target: " << compiler().hostCpu() << " " << compiler().hostFeatures().substr(0, 400));
  CHECK(!compiler().hostCpu().empty());
  CHECK(compiler().hostFeatures().find("+avx2") != std::string::npos);
  CHECK(compiler().hostFeatures().find("+fma") != std::string::npos);
}

TEST_CASE("jit: every corpus patch matches the interpreter (1e-6) and the reference evaluator (1e-5)") {
  const CorpusStats cs = runCorpus({}, false, kBlocks, std::size(kBlocks));
  MESSAGE("JIT (default pipeline, contract): compared " << cs.patches << " patches from " << cs.files << " files (" << cs.voicePatches
                                          << " with voice stages, 11 voices) × 2 param sets × blocks 1/37/64/1024 = "
                                          << cs.configs << " runs of " << kFrames << " frames; max error vs interpreter "
                                          << cs.maxVsInterp << " at " << cs.worstInterp << "; vs reference " << cs.maxVsRef
                                          << " at " << cs.worstRef);
  CHECK(cs.patches >= 20);
  CHECK(cs.voicePatches >= 6);
}

TEST_CASE("jit: without contraction the JIT is bit-identical to the interpreter on the whole corpus") {
  jit::CompileOptions jo;
  jo.contract = false;
  const CorpusStats cs = runCorpus(jo, true, kBlocks, std::size(kBlocks));
  MESSAGE("JIT (default pipeline, no fast-math flags): " << cs.configs << " runs bit-identical to the interpreter; max error vs reference "
                                           << cs.maxVsRef);
  CHECK(cs.maxVsInterp == 0.0);
}

TEST_CASE("jit: every optimisation level is correct") {
  const uint32_t blocks[] = {37};
  for (uint32_t level : {0u, 2u, 3u}) {  // 1 (the default) is the corpus test above
    jit::CompileOptions jo;
    jo.optLevel = level;
    const CorpusStats cs = runCorpus(jo, false, blocks, 1);
    MESSAGE("JIT O" << level << ": " << cs.configs << " runs, max error vs interpreter " << cs.maxVsInterp);
  }
  jit::CompileOptions bad;
  bad.optLevel = 4;
  const Compiled c = compileText("patch P { out y : audio\n y = sine(phasor(440Hz)) }");
  CHECK_FALSE(compileJit(c.sched, bad).ok());
}

TEST_CASE("jit: every simple stateful atom inside a per-sample loop (inline per-frame code), global and per voice") {
  std::vector<std::string> skipped;
  const auto compiled = compileFile(jitPatchesDir() / "stateful_loops.pqp", 48000.0, skipped);
  REQUIRE_MESSAGE(skipped.empty(), skipped.front());
  REQUIRE(compiled.size() == 2);
  for (const Compiled& c : compiled) {
    // The atoms really are in a per-sample loop (so the JIT generates them inline, not as kernel calls).
    std::vector<pq::dsp::Atom> inLoop;
    for (const Region& r : c.sched.regions)
      if (r.kind == RegionKind::Loop && r.subBlock == 1)
        for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) inLoop.push_back(c.sched.ops[i].kernel);
    using A = pq::dsp::Atom;
    for (A a : {A::Schmitt, A::Rise, A::Fall, A::Change, A::SrFlipFlop, A::DFlipFlop, A::TFlipFlop, A::Counter,
                A::ClockDiv, A::Pulse, A::Timer, A::SampleHold, A::Latch, A::Phasor, A::SineHz, A::Mux, A::Demux,
                A::TanhFast, A::Tanh, A::SinFast, A::Sin, A::Exp, A::ExpFast, A::Z1Read, A::Z1Write}) {
      INFO(c.name << " atom " << pq::dsp::kernelInfo(a).name);
      CHECK(std::find(inLoop.begin(), inLoop.end(), a) != inLoop.end());
    }
    jit::CompileStats st;
    REQUIRE(compiler().compile(c.sched, {}, &st).ok());
    const uint32_t voices = voicesFor(c);
    // Nothing in the loops is a call; the voice patch calls its oscillator's phasor kernel (block region, outside the
    // loop) and the voice_sum helper.
    CHECK(st.calledOps == (voices ? 2u : 0u));
    CorpusStats cs;
    for (int paramSet = 0; paramSet < 2; ++paramSet) checkPatch(c, voices, paramSet, {}, cs, false, kBlocks, std::size(kBlocks));
    jit::CompileOptions exact;
    exact.contract = false;
    checkPatch(c, voices, 0, exact, cs, true, kBlocks, std::size(kBlocks));
    MESSAGE(c.name << ": max error vs interpreter " << cs.maxVsInterp << ", vs reference " << cs.maxVsRef);
  }
}

TEST_CASE("jit: voice stages with 1, 8 and 16 voices (reducers, broadcasts, per-group buffers, sends)") {
  std::vector<std::string> skipped;
  const auto compiled = compileFile(patchesDir() / "interp.pqp", 48000.0, skipped);
  REQUIRE(skipped.empty());
  CorpusStats cs;
  for (const Compiled& c : compiled) {
    if (voicesFor(c) == 0) continue;
    for (uint32_t voices : {1u, 8u, 16u}) checkPatch(c, voices, 0, {}, cs, false, kBlocks, std::size(kBlocks));
  }
  CHECK(cs.configs > 0);
}

TEST_CASE("jit: no voices → voice stages do not run and reducers read 0") {
  std::vector<std::string> skipped;
  const Compiled c = compileFile(patchesDir() / "interp.pqp", 48000.0, skipped).at(3);
  REQUIRE(c.name == "VoiceMix");
  const Setup st(c, 0, 0);
  const InterpRun run = runInterp(st, 256, 64, 1024, jitBuilder());
  REQUIRE(run.error.empty());
  CHECK(run.rtViolations == 0);
  for (const auto& [key, stream] : run.streams)
    for (double x : stream) CHECK(x == 0.0);
}

TEST_CASE("jit: params are clamped to their declared range, NaN reads the default, bools are masks") {
  const Compiled c = compileText(R"(patch P {
  param p = 0.25 [0..1]
  param q = 3
  param on : bool = false
  out y : real
  out z : real
  out b : bool
  y = p
  z = q
  b = on
})");
  auto img = compileJit(c.sched);
  REQUIRE(img.ok());
  eng::ProgramImage& im = **img;
  REQUIRE(im.io().params.size() == 3);
  REQUIRE(im.io().globalOut.size() == 3);
  std::vector<float> o0(8), o1(8), o2(8);
  float* outs[3] = {o0.data(), o1.data(), o2.data()};
  eng::ProcessContext ctx;
  ctx.frames = 8;
  ctx.out = outs;
  ctx.numOut = 3;
  auto run = [&](float p, float q, float on) {
    const float params[3] = {p, q, on};
    ctx.params = params;
    ctx.numParams = 3;
    CHECK(pq::alloc_trap::countRtViolations([&] { im.process(ctx); }) == 0);
  };
  run(5.0f, -7.0f, 1.0f);
  CHECK(o0[7] == 1.0f);
  CHECK(o1[7] == -7.0f);  // no range: unclamped
  CHECK(pq::dsp::maskIsTrue(o2[0]));
  run(-2.0f, NAN, 0.0f);
  CHECK(o0[0] == 0.0f);
  CHECK(o1[0] == 3.0f);
  CHECK(!pq::dsp::maskIsTrue(o2[0]));
  run(NAN, 1.0f, 0.0f);
  CHECK(o0[3] == 0.25f);
  ctx.params = nullptr;  // no param block: defaults
  im.process(ctx);
  CHECK(o0[0] == 0.25f);
  CHECK(o1[0] == 3.0f);
}

TEST_CASE("jit: a block above maxBlockFrames is rejected with silent outputs; frames = 0 does nothing") {
  const Compiled c = compileText("patch P { out y : audio\n y = sine(phasor(440Hz)) }");
  jit::CompileOptions o;
  o.maxBlockFrames = 32;
  auto img = compileJit(c.sched, o);
  REQUIRE(img.ok());
  std::vector<float> out(64, 7.0f);
  float* outs[1] = {out.data()};
  eng::ProcessContext ctx;
  ctx.out = outs;
  ctx.numOut = 1;
  ctx.frames = 64;
  (*img)->process(ctx);
  CHECK((*img)->rejectedBlocks() == 1);
  for (float x : out) CHECK(x == 0.0f);
  ctx.frames = 0;
  out.assign(64, 7.0f);
  (*img)->process(ctx);
  CHECK(out[0] == 7.0f);
  ctx.frames = 32;
  (*img)->process(ctx);
  CHECK(out[0] == 0.0f);  // phase 0
  CHECK(out[1] != 0.0f);
  CHECK(out[40] == 7.0f);
}

TEST_CASE("jit: missing engine inputs read silence; an image's interface lists its slots") {
  const Compiled c = compileText(R"(patch P {
  in a : audio
  in k : real param
  out y : audio
  export e = a * 2
  y = a + k + track.in[0]
})");
  auto img = compileJit(c.sched);
  REQUIRE(img.ok());
  const eng::ImageInterface& io = (*img)->io();
  REQUIRE(io.globalIn.size() == 2);
  CHECK(io.globalIn[0].rate == Rate::Audio);
  CHECK(io.globalIn[1].rate == Rate::Param);
  REQUIRE(io.globalOut.size() == 2);
  CHECK(io.sampleRate == 48000.0);
  std::vector<float> y(16, 5.0f), e(16, 5.0f);
  float* outs[2] = {y.data(), e.data()};
  const bool exportFirst = io.globalOut[0].isExport;
  eng::ProcessContext ctx;
  ctx.frames = 16;
  ctx.out = outs;
  ctx.numOut = 2;
  CHECK(pq::alloc_trap::countRtViolations([&] { (*img)->process(ctx); }) == 0);
  for (int i = 0; i < 16; ++i) {
    CHECK(y[i] == 0.0f);
    CHECK(e[i] == 0.0f);
  }
  std::vector<float> a(16, 0.5f), k(16, 0.25f);
  k[3] = 9.0f;  // param rate: only frame 0 is read
  const float* ins[2] = {a.data(), k.data()};
  ctx.in = ins;
  ctx.numIn = 2;
  (*img)->process(ctx);
  float* yy = exportFirst ? e.data() : y.data();
  float* ee = exportFirst ? y.data() : e.data();
  for (int i = 0; i < 16; ++i) {
    CHECK(yy[i] == 0.75f);
    CHECK(ee[i] == 1.0f);
  }
}

TEST_CASE("jit: build errors are reported, not crashed on") {
  Compiled c = compileText("patch P { in x : audio\n out y : audio\n y = x * 2 }");
  jit::CompileOptions bad;
  bad.maxBlockFrames = 0;
  CHECK_FALSE(compileJit(c.sched, bad).ok());
  Schedule broken = c.sched;
  REQUIRE(!broken.ops.empty());
  broken.ops[0].splMask ^= 1u;  // the Spl mask no longer matches the operand
  CHECK_FALSE(compileJit(broken).ok());
  Schedule broken2 = c.sched;
  broken2.operands[broken2.ops[0].firstOperand].index = 9999;
  CHECK_FALSE(compileJit(broken2).ok());
  CHECK(compileJit(c.sched).ok());  // the compiler is still usable afterwards
}

TEST_CASE("jit: state arena and layout are exposed for state transfer") {
  const Compiled c = compileText("patch P { out y : audio\n lp = z1(lp * 0.5 + 1)\n y = lp }");
  auto img = compileJit(c.sched);
  REQUIRE(img.ok());
  const eng::StateArena a = (*img)->stateArena();
  REQUIRE(a.global != nullptr);
  REQUIRE((*img)->stateLayout().entries.size() == 1);
  const StateEntry& e = (*img)->stateLayout().entries[0];
  std::vector<float> y(4);
  float* outs[1] = {y.data()};
  eng::ProcessContext ctx;
  ctx.frames = 4;
  ctx.out = outs;
  ctx.numOut = 1;
  (*img)->process(ctx);
  CHECK(y[0] == 0.0f);
  CHECK(y[1] == 1.0f);
  CHECK(y[2] == 1.5f);
  CHECK(y[3] == 1.75f);
  float last = 0;
  std::memcpy(&last, a.global + e.offset, sizeof last);
  CHECK(last == 1.875f);  // z1's stored input
}

TEST_CASE("jit: denormals flush under DenormalGuard exactly as in the interpreter; generated code leaves MXCSR alone") {
  // A decaying one-pole (per-sample loop) and a block-wise product chain both pass through the denormal range.
  const Compiled c = compileText(R"(patch D {
  in x : audio
  out y : audio
  out z : audio
  lp = z1(lp * 0.5 + x)
  y = lp
  z = x * 1e-30 * 1e-10
})");
  auto jitImg = compileJit(c.sched);
  auto interpImg = eng::buildInterpreted(c.sched);
  REQUIRE(jitImg.ok());
  REQUIRE(interpImg.ok());
  for (bool guard : {true, false}) {
    std::vector<float> x(512, 0.0f), yj(512), zj(512), yi(512), zi(512);
    x[0] = 1.0f;
    for (size_t i = 0; i < 64; ++i) x[i] = 1.0f;  // z = 1e-40: denormal unless flushed
    auto run = [&](eng::ProgramImage& im, std::vector<float>& y, std::vector<float>& z) {
      const float* ins[1] = {x.data()};
      float* outs[2] = {y.data(), z.data()};
      eng::ProcessContext ctx;
      ctx.frames = 512;
      ctx.in = ins;
      ctx.numIn = 1;
      ctx.out = outs;
      ctx.numOut = 2;
      const unsigned before = _mm_getcsr() & ~0x3Fu;  // control bits (the exception flags are sticky status)
      if (guard) {
        pq::DenormalGuard dg;
        im.process(ctx);
      } else {
        im.process(ctx);
      }
      CHECK((_mm_getcsr() & ~0x3Fu) == before);
    };
    run(**jitImg, yj, zj);
    run(**interpImg, yi, zi);
    for (size_t i = 0; i < 512; ++i) {
      CHECK(std::memcmp(&yj[i], &yi[i], sizeof(float)) == 0);
      CHECK(std::memcmp(&zj[i], &zi[i], sizeof(float)) == 0);
    }
    if (guard) {
      CHECK(zj[0] == 0.0f);    // flushed
      CHECK(yj[300] == 0.0f);  // the decay reached zero instead of crawling through denormals
    } else {
      CHECK(zj[0] != 0.0f);    // IEEE: a denormal
    }
  }
}

TEST_CASE("jit: an image outlives its compiler and frees its code") {
  const Compiled c = compileText("patch P { in x : audio\n out y : audio\n y = tanh(x * 3) }");
  std::unique_ptr<eng::ProgramImage> img;
  {
    auto own = jit::Compiler::create();
    REQUIRE(own.ok());
    auto r = (*own)->compile(c.sched);
    REQUIRE(r.ok());
    img = std::move(r.value());
  }
  std::vector<float> x(16, 0.25f), y(16);
  const float* ins[1] = {x.data()};
  float* outs[1] = {y.data()};
  eng::ProcessContext ctx;
  ctx.frames = 16;
  ctx.in = ins;
  ctx.numIn = 1;
  ctx.out = outs;
  ctx.numOut = 1;
  img->process(ctx);
  CHECK(y[15] == doctest::Approx(std::tanh(0.75)).epsilon(1e-6));
  img.reset();
}

TEST_CASE("ImageRunner: a mono JIT patch through BlockAdapter plays on every channel, scaled and limited") {
  const Compiled c = compileText("patch P { param g = 4 [0..8]\n out y : audio\n y = sine(phasor(1kHz)) * g }");
  jit::CompileOptions o;
  o.maxBlockFrames = 32;
  auto img = compileJit(c.sched, o);
  REQUIRE(img.ok());
  eng::ImageRunner runner;
  REQUIRE(runner.prepare(img->get(), 0, 2, 0.5f, true));
  eng::BlockAdapter adapter;
  REQUIRE(adapter.prepare(runner.callback(), 0, 2, 32, 100));  // 100-frame device buffers: FIFO mode
  std::vector<float> l(100), r(100);
  float* outs[2] = {l.data(), r.data()};
  float peak = 0;
  for (int b = 0; b < 10; ++b) {
    const eng::AudioBlock blk{nullptr, 0, outs, 2, 100, b * 100};
    CHECK(pq::alloc_trap::countRtViolations([&] {
            pq::DenormalGuard dg;
            adapter.deviceCallback().fn(adapter.deviceCallback().user, blk);
          }) == 0);
    for (int i = 0; i < 100; ++i) {
      CHECK(l[i] == r[i]);
      peak = std::max(peak, std::fabs(l[i]));
    }
  }
  CHECK(peak == 1.0f);  // 4 · 0.5 = 2 → limited to 1
  CHECK(runner.blocks() > 0);
}

TEST_CASE("ImageRunner on NullDevice: device → CallbackRunner → BlockAdapter → JIT comb filter") {
  const Compiled c = compileText(R"(patch Comb {
  in x : audio
  param fb = 0.7 [0..0.95]
  out y : audio
  c = delay(x + c * fb, 2.5ms, 10ms)
  y = c
})");
  jit::CompileOptions o;
  o.maxBlockFrames = 32;
  auto img = compileJit(c.sched, o);
  REQUIRE(img.ok());
  eng::NullDevice dev;
  eng::BlockAdapter adapter;
  eng::ImageRunner runner;
  eng::StreamConfig cfg;
  cfg.inputChannels = 1;
  cfg.outputChannels = 2;
  cfg.bufferFrames = 96;
  auto info = dev.open(cfg, adapter.deviceCallback());
  REQUIRE(info.ok());
  REQUIRE(runner.prepare(img->get(), 1, 2));
  REQUIRE(adapter.prepare(runner.callback(), 1, 2, 32, 96));
  std::vector<float> y;
  for (int b = 0; b < 5; ++b) {
    for (uint32_t i = 0; i < 96; ++i) dev.input(0)[i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
    dev.processBlock();  // RtScope + DenormalGuard inside CallbackRunner: an allocation would fail this test
    for (uint32_t i = 0; i < 96; ++i) {
      CHECK(dev.output(0)[i] == dev.output(1)[i]);
      y.push_back(dev.output(0)[i]);
    }
  }
  CHECK(y[119] == 0.0f);
  CHECK(y[120] == 1.0f);
  CHECK(y[121] == 0.0f);
  CHECK(y[240] == doctest::Approx(0.7f));
  CHECK(y[360] == doctest::Approx(0.49f));
  dev.close();
}

TEST_CASE("jit: compile time of 100- and 1000-node patches (and they match the interpreter)") {
  for (int n : {100, 1000}) {
    const Compiled c = compileText(mixedChain(n));
    uint32_t nodes = 0;
    for (NodeIndex k = 0; k < c.typed.patch.nodeCount(); ++k) nodes += c.typed.patch.node(k).kind == NodeKind::Atom;
    for (uint32_t level : {1u, 2u, 3u}) {
      jit::CompileOptions jo;
      jo.optLevel = level;
      jit::CompileStats stats;
      double best = 1e30;
      jit::CompileStats bestStats;
      for (int rep = 0; rep < 3; ++rep) {
        auto img = compiler().compile(c.sched, jo, &stats);
        REQUIRE(img.ok());
        if (stats.totalMs < best) {
          best = stats.totalMs;
          bestStats = stats;
        }
      }
      MESSAGE(n << "-step chain (" << nodes << " atom nodes, " << c.sched.ops.size() << " ops) O" << level << ": total "
                << bestStats.totalMs << " ms (host " << bestStats.hostMs << ", IR " << bestStats.codegenMs << ", opt "
                << bestStats.optimiseMs << ", machine code " << bestStats.machineCodeMs << "); " << bestStats.inlinedOps
                << " ops inline, " << bestStats.calledOps << " called, " << bestStats.irInstructions << " IR instructions");
      CHECK(bestStats.totalMs < 30000.0);
    }
    CorpusStats cs;
    const uint32_t blocks[] = {64};
    checkPatch(c, 0, 0, {}, cs, false, blocks, 1);
  }
}
