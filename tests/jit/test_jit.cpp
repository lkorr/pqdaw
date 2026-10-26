// T1b.1: the LLVM JIT backend. Every T1.7 interpreter test, run through the JIT: the whole corpus (every patch of
// tests/graph/cases, tests/engine/patches and tests/jit/patches × 2 param sets × blocks 1/37/64/1024) bit-identical to
// the interpreter with the default options (T1b.1-fix1: contraction off) and within 1e-5 of the reference evaluator;
// opt-in contraction within 1e-6 on everything but the chaotic probes; every optimisation level; the ProgramImage
// contract cases (params, rejected blocks, missing inputs, interface, build errors, state arena, ImageRunner,
// NullDevice); RtScope around every process(); Newton probes; non-mask bools; compile time (100/1000 nodes, the
// 1200-input mux) and no stack probes; T1.x-arch1: a per-track project on JIT units and an interpreter → JIT unit
// swap after an edit.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "../engine/kernel_probes.h"
#include "../engine/liveness_probes.h"
#include "../engine/newton_probes.h"
#include "../engine/project_fixture.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/note_list.h"
#include "pq/engine/null_device.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/voices.h"
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

ImageBuilder jitBuilder(jit::CompileOptions base = {}, jit::Compiler* with = nullptr) {
  return [base, with](const Schedule& s, const eng::InterpretOptions& io) {
    jit::CompileOptions o = base;
    o.maxBlockFrames = io.maxBlockFrames;
    o.maxVoiceGroups = io.maxVoiceGroups;
    o.resources = io.resources;
    o.sleep = io.sleep;
    return (with ? *with : compiler()).compile(s, o);
  };
}

// test_voices.cpp (compiled into this executable too) builds its VoiceEngine images with the JIT here.
const bool kVoiceRigsUseJit = (rigImageBuilder() = jitBuilder(), true);

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

// tests/jit/patches/fold_scan.pqp multiplies one-ulp differences of exact transcendentals by 1e9; the reference
// evaluator computes them independently in double (a few ulp from the kernels, ref_eval.h), so only the backends'
// bit identity (here) and the probe's own exact zeros (its test below) apply to it.
bool ulpProbe(const Compiled& c) { return c.name == "foldscan"; }

// One patch × voices × param set × every block size: JIT vs interpreter and vs the reference.
void checkPatch(const Compiled& c, uint32_t voices, int paramSet, const jit::CompileOptions& jo, CorpusStats& cs,
                bool requireBitIdentical, const uint32_t* blocks, size_t numBlocks, jit::Compiler* with = nullptr) {
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
    const InterpRun run = runInterp(st, kFrames, block, kMaxBlock, jitBuilder(jo, with));
    REQUIRE_MESSAGE(run.error.empty(), run.error);
    CHECK(run.rtViolations == 0);
    const Mismatch mi = compare(c, interp.streams, run.streams, (*face)->io());
    const Mismatch mr = compare(c, ref, run.streams, (*face)->io());
    CHECK_MESSAGE(mi.maxErr <= kTolInterp, "vs interpreter: max error " << mi.maxErr << ": " << mi.where);
    if (!ulpProbe(c)) CHECK_MESSAGE(mr.maxErr <= kTolRef, "vs reference: max error " << mr.maxErr << ": " << mr.where);
    if (requireBitIdentical) CHECK_MESSAGE(bitDifferences(interp.streams, run.streams) == 0, "not bit-identical: " << mi.where);
    if (mi.maxErr >= cs.maxVsInterp) {
      cs.maxVsInterp = mi.maxErr;
      cs.worstInterp = c.file + ":" + c.name + " " + mi.where + " (block " + std::to_string(block) + ")";
    }
    if (!ulpProbe(c) && mr.maxErr >= cs.maxVsRef) {
      cs.maxVsRef = mr.maxErr;
      cs.worstRef = c.file + ":" + c.name + " " + mr.where + " (block " + std::to_string(block) + ")";
    }
    ++cs.configs;
  }
}

// The M1 review's chaotic feedback probes (tests/jit/patches/review_probes.pqp): any one-ulp difference grows to
// O(1), so only bit-identical backends agree on them.
bool chaotic(const Compiled& c) { return c.name == "Chaos" || c.name == "ChaosVoice"; }

CorpusStats runCorpus(const jit::CompileOptions& jo, bool requireBitIdentical, const uint32_t* blocks, size_t numBlocks,
                      bool includeChaotic = true) {
  CorpusStats cs;
  std::vector<std::string> skipped;
  for (const fs::path& path : allPatchFiles()) {
    ++cs.files;
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      if (!includeChaotic && chaotic(c)) continue;
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

// docs/reviews/M1/probes/mux1200.pqp: one mux over n products of a sine (one fused run of n + 1 ops before
// T1b.1-fix1, interleaved 8 times: 1.3–3 s of machine code).
std::string bigMux(int n) {
  std::string t = "patch BigMux {\n  out y : audio\n  p = phasor(3Hz)\n  x = sine(phasor(220Hz))\n";
  std::string args;
  for (int i = 0; i < n; ++i) {
    t += "  d" + std::to_string(i) + " = x * " + std::to_string(1.0 - i / static_cast<double>(n)) + "\n";
    args += ", d" + std::to_string(i);
  }
  return t + "  y = mux(p * " + std::to_string(n) + args + ")\n}\n";
}

// The compile-size proxy's bounds (see its test case), about 1.5x what T1.11-fix1 measured (2026-10-25, default
// CompileOptions): 100-step chain 1436 IR instructions / 18666 B of code, 1200-input mux 9897 / 214474 B. A JIT change
// that legitimately grows the code more than that updates them (and says so in PROGRESS).
constexpr uint32_t kChainIrBound = 2200, kMuxIrBound = 15000;
constexpr uint64_t kChainCodeBound = 28000, kMuxCodeBound = 320000;

// Best total compile time of `reps` compiles.
jit::CompileStats bestCompile(const Schedule& s, const jit::CompileOptions& jo, int reps) {
  jit::CompileStats best;
  best.totalMs = 1e30;
  for (int r = 0; r < reps; ++r) {
    jit::CompileStats st;
    auto img = compiler().compile(s, jo, &st);
    REQUIRE(img.ok());
    if (st.totalMs < best.totalMs) best = st;
  }
  return best;
}

}  // namespace

TEST_CASE("jit: host CPU and features") {
  MESSAGE("JIT target: " << compiler().hostCpu() << " " << compiler().hostFeatures().substr(0, 400));
  CHECK(!compiler().hostCpu().empty());
  CHECK(compiler().hostFeatures().find("+avx2") != std::string::npos);
  CHECK(compiler().hostFeatures().find("+fma") != std::string::npos);
}

TEST_CASE("jit: every corpus patch is bit-identical to the interpreter and matches the reference evaluator (1e-5)" * doctest::test_suite("slow")) {
  const CorpusStats cs = runCorpus({}, true, kBlocks, std::size(kBlocks));
  MESSAGE("JIT (default options: light pipeline, no fast-math flags): compared " << cs.patches << " patches from " << cs.files
                                          << " files (" << cs.voicePatches << " with voice stages, 11 voices) × 2 param sets × "
                                          << "blocks 1/37/64/1024 = " << cs.configs << " runs of " << kFrames
                                          << " frames, bit-identical to the interpreter; max error vs reference " << cs.maxVsRef
                                          << " at " << cs.worstRef);
  CHECK(cs.maxVsInterp == 0.0);
  CHECK(cs.patches >= 30);
  CHECK(cs.voicePatches >= 8);
}

TEST_CASE("jit: opt-in contraction stays within 1e-6 of the interpreter except in chaotic feedback" * doctest::test_suite("slow")) {
  jit::CompileOptions jo;
  jo.contract = true;
  const CorpusStats cs = runCorpus(jo, false, kBlocks, std::size(kBlocks), false);
  MESSAGE("JIT (contract + afn): " << cs.configs << " runs; max error vs interpreter " << cs.maxVsInterp << " at " << cs.worstInterp
                                   << "; vs reference " << cs.maxVsRef);
}

TEST_CASE("jit: fold_scan: folded literals equal the runtime value at param rate (8 lanes) and global audio rate (1 lane), both backends") {
  // M1 review M4 + QUESTIONS Q94: constant folding runs the 8-lane kernel; Control regions run it too; a global
  // audio-rate op runs the 1-lane kernel, which calls the same SVML functions (dsp simd.h S1). Every channel of the
  // probe is (folded − runtime) · 1e9 with the default params, so it is exactly 0 at every sample.
  std::vector<std::string> skipped;
  const std::vector<Compiled> compiled = compileFile(jitPatchesDir() / "fold_scan.pqp", 48000.0, skipped);
  REQUIRE(compiled.size() == 1);
  const Compiled& c = compiled[0];
  const Setup st(c, 0, 0);
  for (const bool jit : {false, true}) {
    for (const uint32_t block : {1u, 64u}) {
      CAPTURE(jit);
      CAPTURE(block);
      const InterpRun run = runInterp(st, 256, block, kMaxBlock, jit ? jitBuilder() : ImageBuilder{});
      REQUIRE_MESSAGE(run.error.empty(), run.error);
      REQUIRE(run.streams.size() == 2 * 38);
      std::string nonzero;
      for (const auto& [key, x] : run.streams) {
        const bool zero = std::all_of(x.begin(), x.end(), [](double v) { return v == 0.0; });
        if (!zero)  // global out slots: y's 38 channels, then z's
          nonzero += " " + std::string(key.a < 38 ? "y" : "z") + "[" + std::to_string(key.a % 38) + "]=" + std::to_string(x.front());
      }
      CHECK_MESSAGE(nonzero.empty(), "folded != runtime on:" << nonzero);
    }
  }
}

TEST_CASE("jit: the M1 review's chaotic feedback probes are bit-identical to the interpreter (contraction off by default)") {
  std::vector<std::string> skipped;
  const auto compiled = compileFile(jitPatchesDir() / "review_probes.pqp", 48000.0, skipped);
  REQUIRE_MESSAGE(skipped.empty(), skipped.front());
  uint32_t n = 0;
  for (const Compiled& c : compiled) {
    if (!chaotic(c)) continue;
    ++n;
    const Setup st(c, voicesFor(c), 0);
    const InterpRun interp = runInterp(st, kFrames, 64, kMaxBlock);
    REQUIRE(interp.error.empty());
    const InterpRun jitRun = runInterp(st, kFrames, 64, kMaxBlock, jitBuilder());
    REQUIRE(jitRun.error.empty());
    INFO(c.name);
    CHECK(bitDifferences(interp.streams, jitRun.streams) == 0);
    // Opting into contraction fuses r − r·fb into one FMA, and the map turns that ulp into an O(1) difference:
    // the reason it is not the default (and evidence that the option does what it says).
    jit::CompileOptions jo;
    jo.contract = true;
    const InterpRun fused = runInterp(st, kFrames, 64, kMaxBlock, jitBuilder(jo));
    REQUIRE(fused.error.empty());
    auto face = eng::buildInterpreted(c.sched);
    REQUIRE(face.ok());
    const Mismatch m = compare(c, interp.streams, fused.streams, (*face)->io());
    MESSAGE(c.name << " with contraction: max |jit - interp| " << m.maxErr);
    CHECK(m.maxErr > 0.01);
  }
  CHECK(n == 2);
}

TEST_CASE("jit: Newton probes converge and are bit-identical to the interpreter (tangents, finite differences, fallback)") {
  newton_probes::check(jitBuilder());
  for (const Compiled& c : newton_probes::probes()) {
    INFO(c.name);
    CHECK(liveness_probes::bitEqual(newton_probes::run(c, 512, 64, {}).streams, newton_probes::run(c, 512, 64, jitBuilder()).streams));
  }
}

TEST_CASE("jit: every optimisation level is bit-identical to the interpreter" * doctest::test_suite("slow")) {
  const uint32_t blocks[] = {37};
  for (uint32_t level : {0u, 2u, 3u}) {  // 1 (the default) is the corpus test above
    jit::CompileOptions jo;
    jo.optLevel = level;
    const CorpusStats cs = runCorpus(jo, true, blocks, 1);
    MESSAGE("JIT O" << level << ": " << cs.configs << " runs, max error vs interpreter " << cs.maxVsInterp);
    CHECK(cs.maxVsInterp == 0.0);
  }
  for (uint32_t cg = 0; cg <= 3; ++cg) {  // every machine-code level (CompilerConfig::codeGenLevel)
    auto own = jit::Compiler::create({cg});
    REQUIRE(own.ok());
    CorpusStats cs;
    std::vector<std::string> skipped;
    for (const fs::path& path : allPatchFiles())
      for (const Compiled& c : compileFile(path, 48000.0, skipped))
        checkPatch(c, voicesFor(c), 0, {}, cs, true, blocks, 1, own->get());
    MESSAGE("JIT codeGenLevel " << cg << ": " << cs.configs << " runs, max error vs interpreter " << cs.maxVsInterp);
    CHECK(cs.maxVsInterp == 0.0);
  }
  CHECK_FALSE(jit::Compiler::create({4}).ok());
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
    // Nothing in the loops is a call; the voice patch calls the voice_sum helper (its oscillator's phasor in the
    // block region outside the loop is inline too since T1b.2).
    CHECK(st.calledOps == (voices ? 1u : 0u));
    CorpusStats cs;
    for (int paramSet = 0; paramSet < 2; ++paramSet) checkPatch(c, voices, paramSet, {}, cs, false, kBlocks, std::size(kBlocks));
    jit::CompileOptions exact;
    exact.contract = false;
    checkPatch(c, voices, 0, exact, cs, true, kBlocks, std::size(kBlocks));
    MESSAGE(c.name << ": max error vs interpreter " << cs.maxVsInterp << ", vs reference " << cs.maxVsRef);
  }
}

// T1b.2: the ways of generating voice stages (one group per call; two groups per call on 256-bit halves or 512-bit
// registers; the scalar-lane baseline) all compute the interpreter's bits. 19 voices = groups 0 and 1 as a pair,
// group 2 (three voices) alone.
static void voiceCodeCorpus(jit::VoiceCode vc, const uint32_t* blocks, size_t numBlocks, int paramSets) {
  CorpusStats cs;
  uint32_t patches = 0, paired = 0, scalarRefused = 0;
  std::vector<std::string> skipped;
  for (const fs::path& path : allPatchFiles())
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      if (voicesFor(c) == 0) continue;
      jit::CompileOptions jo;
      jo.voiceCode = vc;
      jo.maxVoiceGroups = 3;
      jit::CompileStats st;
      auto img = compiler().compile(c.sched, jo, &st);
      if (vc == jit::VoiceCode::ScalarLanes && !img) {  // a voice stage with kernel / special-op calls
        CHECK(img.error().message.find("scalar-lane baseline") != std::string::npos);
        ++scalarRefused;
        continue;
      }
      if (!img) FAIL(c.file << ":" << c.name << ": " << img.error().message);
      CHECK_FALSE(st.stackProbes);
      paired += st.pairedStages > 0;
      ++patches;
      for (int ps = 0; ps < paramSets; ++ps) checkPatch(c, 19, ps, jo, cs, true, blocks, numBlocks);
    }
  MESSAGE("voice code " << static_cast<int>(vc) << ": " << patches << " voice patches (" << paired << " with pair functions, "
                        << scalarRefused << " refused by the scalar baseline), " << cs.configs
                        << " runs bit-identical to the interpreter");
  CHECK(cs.maxVsInterp == 0.0);
  CHECK(patches >= (vc == jit::VoiceCode::ScalarLanes ? 3u : 8u));
  if (vc == jit::VoiceCode::Pairs256 || vc == jit::VoiceCode::Pairs512) CHECK(paired >= 5);
  if (vc == jit::VoiceCode::Groups || vc == jit::VoiceCode::ScalarLanes) CHECK(paired == 0);
}

TEST_CASE("jit: voice code: groups, pairs (256-bit halves, 512-bit registers) and the scalar lanes are bit-identical to the interpreter") {
  const uint32_t blocks[] = {37};
  MESSAGE("host AVX-512F: " << (compiler().hostFeatures().find("+avx512f") != std::string::npos));
  for (jit::VoiceCode vc : {jit::VoiceCode::Groups, jit::VoiceCode::Pairs256, jit::VoiceCode::Pairs512, jit::VoiceCode::ScalarLanes})
    voiceCodeCorpus(vc, blocks, 1, 1);
}

TEST_CASE("jit: voice code, every block size and param set (19 voices)" * doctest::test_suite("slow")) {
  for (jit::VoiceCode vc : {jit::VoiceCode::Groups, jit::VoiceCode::Pairs256, jit::VoiceCode::Pairs512, jit::VoiceCode::ScalarLanes})
    voiceCodeCorpus(vc, kBlocks, std::size(kBlocks), 2);
}

TEST_CASE("jit: the scalar-lane baseline refuses a voice that calls kernels; a pair function exists only where it pays") {
  // A voice delay line is a kernel call per block: the baseline cannot run it lane by lane.
  const Compiled echo = compileText(
      "patch E { out y : audio voice\n x = sine(phasor(voice.hz))\n y = x + delay(x, 3ms, 10ms) * 0.5 }");
  jit::CompileOptions scalar;
  scalar.voiceCode = jit::VoiceCode::ScalarLanes;
  auto r = compiler().compile(echo.sched, scalar);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().message.find("delay") != std::string::npos);
  // Oscillator + filter: every voice op inline, a chain (the oscillators fused into the filter's per-sample loop) and
  // a pair function; with one voice group there is nothing to pair.
  const Compiled osc = compileText(R"(patch OscFilter {
  param cutoff = 1500Hz [20Hz..20kHz]
  out   out : audio voice mono
  x   = phasor(voice.hz) * 2 - 1
  g   = tan(3.141592653589793 * cutoff / sr)
  lp  = z1(lp + (x - lp) * (g / (1 + g)))
  out = lp * gate_to_real(voice.gate)
})");
  jit::CompileStats st;
  REQUIRE(compiler().compile(osc.sched, {}, &st).ok());
  CHECK(st.calledOps == 0);
  CHECK(st.pairedStages == 1);
  jit::CompileOptions one;
  one.maxVoiceGroups = 1;
  REQUIRE(compiler().compile(osc.sched, one, &st).ok());
  CHECK(st.pairedStages == 0);
  REQUIRE(compiler().compile(osc.sched, scalar, &st).ok());
  CHECK(st.pairedStages == 0);
  CorpusStats cs;
  for (jit::VoiceCode vc : {jit::VoiceCode::Auto, jit::VoiceCode::ScalarLanes}) {
    jit::CompileOptions jo;
    jo.voiceCode = vc;
    for (uint32_t voices : {1u, 8u, 19u, 64u}) checkPatch(osc, voices, 0, jo, cs, true, kBlocks, std::size(kBlocks));
  }
  CHECK(cs.maxVsInterp == 0.0);
}

TEST_CASE("jit: VoiceEngine-driven FmVoice / echo / probe (note traffic, steals, voice moves) match the interpreter") {
  std::vector<std::string> skipped;
  std::vector<Compiled> patches = compileFile(fs::path(PQ_TEST_DATA_DIR) / "graph" / "cases" / "fm_voice.pqp", 48000.0, skipped);
  REQUIRE(patches.size() == 1);
  patches.push_back(compileText(R"(patch Echo {
  out y : audio voice
  x = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
  e = delay(x + e * 0.6, 3ms, 10ms)
  y = x + e * voice.velocity
})"));
  for (const Compiled& c : patches) {
    std::vector<float> outs[2];
    for (int backend = 0; backend < 2; ++backend) {
      eng::InterpretOptions io;
      io.maxBlockFrames = 64;
      io.maxVoiceGroups = 4;
      auto img = backend == 0 ? eng::buildInterpreted(c.sched, io) : jitBuilder({.contract = false})(c.sched, io);
      REQUIRE(img.ok());
      eng::VoiceEngine ve;
      eng::VoiceConfig cfg;
      cfg.maxVoices = 32;
      cfg.compact = true;
      REQUIRE(ve.prepare(img->get(), cfg).ok());
      REQUIRE(ve.numMixes() >= 1);
      eng::EventBuffer buf(256);
      uint32_t rng = 77, id = 1;
      std::vector<uint32_t> held;
      for (int b = 0; b < 400; ++b) {
        buf.clear();
        for (int k = 0; k < 3; ++k) {
          rng = rng * 1664525u + 1013904223u;
          const uint32_t frame = (rng >> 8) % 64;
          if ((rng >> 20) % 3 != 0 || held.empty()) {
            buf.push(eng::noteOn(frame, id, 60.0f * std::exp2(static_cast<float>((rng >> 4) % 4000) / 1200.0f), 0.6f));
            held.push_back(id++);
          } else {
            buf.push(eng::noteOff(frame, held.front()));
            held.erase(held.begin());
          }
        }
        // events must be in frame order
        std::vector<eng::NoteEvent> evs(buf.list().events, buf.list().events + buf.list().count);
        std::stable_sort(evs.begin(), evs.end(), [](const eng::NoteEvent& a, const eng::NoteEvent& z) { return a.frame < z.frame; });
        buf.clear();
        for (const auto& e : evs) buf.push(e);
        eng::ProcessContext ctx;
        ctx.frames = 64;
        ctx.sampleRate = 48000.0;
        ctx.sampleTime = static_cast<int64_t>(b) * 64;
        ctx.events = &buf.list();
        CHECK(pq::alloc_trap::countRtViolations([&] {
                pq::DenormalGuard dg;
                ve.process(ctx);
              }) == 0);
        outs[backend].insert(outs[backend].end(), ve.mix(0), ve.mix(0) + 64);
      }
    }
    uint64_t diff = 0;
    double energy = 0;
    for (size_t t = 0; t < outs[0].size(); ++t) {
      diff += std::memcmp(&outs[0][t], &outs[1][t], sizeof(float)) != 0;
      energy += outs[0][t] * outs[0][t];
    }
    INFO(c.name);
    CHECK(energy > 1.0);
    CHECK(diff == 0);  // contraction off: bit-identical
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

TEST_CASE("jit: compile time of 100- and 1000-node patches (and they match the interpreter)" * doctest::test_suite("slow")) {
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

TEST_CASE("jit (perf): a 100-node patch compiles in <= 100 ms and the 1200-input mux in <= 300 ms (release, idle machine)" *
          doctest::test_suite("perf")) {
  const Compiled chain = compileText(mixedChain(100));
  const Compiled mux = compileText(bigMux(1200));
  const jit::CompileStats a = bestCompile(chain.sched, {}, 5);
  const jit::CompileStats b = bestCompile(mux.sched, {}, 3);
  MESSAGE("100-step chain (" << chain.sched.ops.size() << " ops): " << a.totalMs << " ms (machine code " << a.machineCodeMs
                             << "); 1200-input mux (" << mux.sched.ops.size() << " ops): " << b.totalMs << " ms (machine code "
                             << b.machineCodeMs << ", " << b.objectBytes << " B of code)");
#ifdef NDEBUG
  CHECK(a.totalMs <= 100.0);
  CHECK(b.totalMs <= 300.0);
#endif
}

TEST_CASE("jit: compile-size proxy for compile time: IR and machine code of the 100-step chain and the 1200-input mux") {
  // M1 review M14: the wall-clock bounds (100-node <= 100 ms, mux1200 <= 300 ms) are checked only in the perf tier,
  // which depends on machine load. Compile time follows the amount of IR and machine code, which does not: the
  // regression M14 found (an 8x interleave duplicating every select of the mux into one huge block) multiplied both.
  // Bounds = about 1.5x what T1.11-fix1 measured (printed below), so a real blow-up fails here in every tier.
  const Compiled chain = compileText(mixedChain(100));
  const Compiled mux = compileText(bigMux(1200));
  jit::CompileStats a, b;
  REQUIRE(compiler().compile(chain.sched, {}, &a).ok());
  REQUIRE(compiler().compile(mux.sched, {}, &b).ok());
  MESSAGE("100-step chain: " << a.irInstructions << " IR instructions, " << a.objectBytes << " B of code; 1200-input mux: "
                             << b.irInstructions << " IR instructions, " << b.objectBytes << " B of code");
  CHECK(a.irInstructions <= kChainIrBound);
  CHECK(a.objectBytes <= kChainCodeBound);
  CHECK(b.irInstructions <= kMuxIrBound);
  CHECK(b.objectBytes <= kMuxCodeBound);
}

TEST_CASE("jit: no stage function needs stack probes (the 1200-input mux, the corpus)") {
  // A frame over 4 KiB makes the function call __chkstk, which touches the guard pages below the stack on the audio
  // thread (M1 review L4: the mux's 1201-operand call arrays were 2 × 9.6 KB in every stage, and one 9600-value
  // fused run spilled far more).
  const Compiled mux = compileText(bigMux(1200));
  jit::CompileStats st;
  REQUIRE(compiler().compile(mux.sched, {}, &st).ok());
  MESSAGE("1200-input mux: " << st.objectBytes << " B of code, " << st.totalMs << " ms");
  CHECK(st.objectBytes > 0);
  CHECK_FALSE(st.stackProbes);
  std::vector<std::string> skipped;
  uint32_t compiled = 0;
  for (const fs::path& path : allPatchFiles())
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      INFO(c.file << ":" << c.name);
      jit::CompileStats cs;
      REQUIRE(compiler().compile(c.sched, {}, &cs).ok());
      CHECK_FALSE(cs.stackProbes);
      ++compiled;
    }
  CHECK(compiled >= 30);
}

TEST_CASE("jit: non-mask bool inputs (1.0f) mean the same in the inline code and the dsp kernels") {
  // Bools are masks (all ones / zero); an engine input that violates that (1.0f) used to select per byte in the dsp
  // kernels (pblendvb) and by the sign bit in the JIT (M1 review L5). Both now select by the sign bit, so 1.0f is false
  // everywhere and the backends agree bit for bit. The loops put the atoms in a per-sample region, where the JIT
  // generates them inline and the interpreter calls the kernels.
  const Compiled c = compileText(R"(patch NonMask {
  in b : bool audio
  out p : audio
  out k : audio
  out q : audio
  out t : audio
  pp = phasor(1000Hz, reset: b || z1(pp) > 2)
  kk = counter(b || z1(kk) > 100, reset: b, wrap: 5)
  qq = gate_to_real(clockdiv(b || z1(qq) > 2, 3))
  tt = gate_to_real(timer(b || z1(tt) > 2, 10))
  p = pp
  k = kk
  q = qq
  t = tt
})");
  std::vector<std::vector<float>> outs[2];
  for (int backend = 0; backend < 2; ++backend) {
    eng::InterpretOptions io;
    io.maxBlockFrames = 64;
    auto img = backend == 0 ? eng::buildInterpreted(c.sched, io) : jitBuilder()(c.sched, io);
    REQUIRE(img.ok());
    REQUIRE((*img)->io().globalOut.size() == 4);
    std::vector<float> in(64);
    std::vector<std::vector<float>> o(4, std::vector<float>(64));
    float* op[4] = {o[0].data(), o[1].data(), o[2].data(), o[3].data()};
    outs[backend].assign(4, {});
    for (int blk = 0; blk < 16; ++blk) {
      for (int f = 0; f < 64; ++f) in[f] = ((blk * 64 + f) / 7) % 2 ? 1.0f : 0.0f;  // 1.0f, not a mask
      const float* ip[1] = {in.data()};
      eng::ProcessContext ctx;
      ctx.frames = 64;
      ctx.in = ip;
      ctx.numIn = 1;
      ctx.out = op;
      ctx.numOut = 4;
      (*img)->process(ctx);
      for (int k = 0; k < 4; ++k) outs[backend][k].insert(outs[backend][k].end(), o[k].begin(), o[k].end());
    }
  }
  uint32_t nonZero = 0;
  for (int k = 0; k < 4; ++k) {
    INFO("output " << k);
    CHECK(std::memcmp(outs[0][k].data(), outs[1][k].data(), outs[0][k].size() * sizeof(float)) == 0);
    for (float x : outs[0][k]) nonZero += x != 0.0f;
  }
  CHECK(nonZero > 0);  // the phasor runs (1.0f has a clear sign bit: never a reset)
}

TEST_CASE("jit: M1 review kernel edge probes (delay size, sample_read rates and loops, mod, timer, NaN hz, fast_sin)") {
  kernel_probes::check(jitBuilder());
  // The inlined mod, timer, phasor/sine_hz and fast_sin and the called kernels match the interpreter bit for bit.
  for (const char* name : {"SrTiny", "LoopFast", "ModTiny", "TimerNan", "PhNan", "FastSinBig"}) {
    const Compiled& c = kernel_probes::probe(name);
    const InterpRun interp = kernel_probes::run(c, 4096, 64, {});
    INFO(name);
    CHECK(liveness_probes::bitEqual(interp.streams, kernel_probes::run(c, 4096, 64, jitBuilder()).streams));
  }
}

TEST_CASE("jit: M1 review liveness probes (solve operands, voice outputs, stereo feedback delay, delay sub-block)") {
  liveness_probes::check(jitBuilder());
  // The stereo feedback delay compiles (it was "op 6: wrong input count") and matches the interpreter bit for bit,
  // with contraction on (the default) and off; so do the other probes.
  for (const char* name : {"StereoFb", "DelayLb", "SolveInplace", "SolveUnk2", "VoiceOut"}) {
    const Compiled& c = liveness_probes::probe(liveness_probes::probes(), name);
    const uint32_t voices = std::string(name) == "VoiceOut" ? 8u : 0u;
    const InterpRun interp = liveness_probes::run(c, voices, 4096, 64, {});
    for (bool contract : {true, false}) {
      INFO(name << " contract " << contract);
      jit::CompileOptions jo;
      jo.contract = contract;
      CHECK(liveness_probes::bitEqual(interp.streams, liveness_probes::run(c, voices, 4096, 64, jitBuilder(jo)).streams));
    }
  }
}

namespace {

pq::Result<std::unique_ptr<eng::ProgramImage>> jitUnit(void*, const Schedule& s, uint32_t maxBlock, uint32_t groups,
                                                       const eng::Resources* res) {
  jit::CompileOptions o;
  o.maxBlockFrames = maxBlock;
  o.maxVoiceGroups = groups;
  o.resources = res;
  o.contract = false;  // bit-identical to the interpreter
  return compiler().compile(s, o);
}

struct ProjectRig {
  std::unique_ptr<project_fixture::Project> proj;
  std::unique_ptr<eng::ProjectCompiler> compiler;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  std::unique_ptr<eng::ProjectProgram> program;
  int64_t time = 0;
  uint64_t rtViolations = 0;

  explicit ProjectRig(bool useJit) {
    const project_fixture::Options o{4, 3, 3, 1};
    proj = project_fixture::makeProject(o);
    REQUIRE(proj != nullptr);
    eng::ProjectCompileOptions co;
    co.sampleRate = 48000.0;
    co.maxBlockFrames = 64;
    co.voice.maxVoices = 8;
    if (useJit) co.backend = {&jitUnit, nullptr};
    compiler = std::make_unique<eng::ProjectCompiler>(co);
    auto b = compiler->build(proj->graph);
    REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
    eng::ProjectConfig pc;
    pc.sampleRate = 48000.0;
    pc.maxBlockFrames = 64;
    for (uint32_t k = 0; k < o.numTracks; ++k) {
      const float hz = 110.0f * (1.0f + 0.25f * static_cast<float>(k));
      notes.push_back(std::make_unique<eng::NoteListSource>(
          std::vector<eng::TimedNote>{{0, 3000, hz, 0.8f, 0.5f}, {1200, 4000, hz * 1.5f, 0.6f, 0.5f}}));
      pc.eventSources.push_back(notes.back()->source());
    }
    auto prog = eng::ProjectProgram::create(pc, b.value().layout);
    REQUIRE(prog.ok());
    program = std::move(prog.value());
    for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(program->publish(u, std::move(b.value().images[u])).ok());
  }
  void render(uint32_t frames, std::vector<float>& out) {
    std::vector<float> l(64), r(64);
    float* outs[2] = {l.data(), r.data()};
    for (uint32_t f = 0; f < frames; f += 64) {
      eng::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 2;
      ab.frames = 64;
      ab.frameTime = time;
      rtViolations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        program->process(ab);
      });
      out.insert(out.end(), l.begin(), l.end());
      time += 64;
    }
  }
  void editLevel(uint32_t track, double level) {
    Patch& tp = *proj->trackPatches[track];
    for (NodeIndex n = 0; n < tp.nodeCount(); ++n)
      if (tp.node(n).kind == NodeKind::Const && tp.constant(n).literal == 0.15) {
        tp.constant(n) = Constant::real(level);
        return;
      }
    FAIL("no level constant");
  }
};

}  // namespace

TEST_CASE("jit: a per-track project on JIT units == interpreted units; an interpreter -> JIT unit swap after an edit") {
  ProjectRig j(true), in(false);
  std::vector<float> a, b;
  j.render(2048, a);
  in.render(2048, b);
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), sizeof(float) * a.size()) == 0);
  // Edit track 1: the JIT project plays the interpreted unit at once, then swaps to its JIT image.
  j.editLevel(1, 0.5);
  in.editLevel(1, 0.5);
  const uint32_t tracks[] = {1};
  const eng::UnitBackend interpreter{};
  auto uj = j.compiler->update(j.proj->graph, tracks, &interpreter);
  auto ui = in.compiler->update(in.proj->graph, tracks);
  REQUIRE(uj.ok());
  REQUIRE(ui.ok());
  REQUIRE(uj.value().size() == 1);
  const uint32_t unit = uj.value()[0].unit;
  REQUIRE(j.program->publish(unit, std::move(uj.value()[0].image)).ok());
  REQUIRE(in.program->publish(unit, std::move(ui.value()[0].image)).ok());
  j.render(1024, a);
  in.render(1024, b);
  auto img = jitUnit(nullptr, *j.compiler->unit(unit).schedule, 64, eng::voiceGroupsFor(8), nullptr);
  REQUIRE(img.ok());
  auto bound = j.compiler->bind(unit, std::move(img.value()));
  REQUIRE(bound.ok());
  REQUIRE(j.program->publish(unit, std::move(bound.value())).ok());
  j.render(2048, a);
  in.render(2048, b);
  CHECK(j.program->unitStats(unit).swaps == 3);
  CHECK(j.program->unitStats(unit).crossfades == 2);
  CHECK(in.program->unitStats(unit).crossfades == 1);
  CHECK(std::memcmp(a.data(), b.data(), sizeof(float) * a.size()) == 0);
  CHECK(j.rtViolations == 0);
  CHECK(in.rtViolations == 0);
}
