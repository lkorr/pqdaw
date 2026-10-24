// T1b.1: LLVM JIT vs interpreter, cost per sample of the T1.7 benchmark patches, and compile times. Release build.
//   chain32        32-node global chain (mul/add/tanh/min…, audio rate), block 64
//   fm_voice ×8    PATCH_LANGUAGE §4 FmVoice (2 phasors, 2 sines, adsr from atoms: a per-sample loop), 8 voices
//   fm_voice ×64   the same, 64 voices = 8 groups
//   fm_osc ×8      FmVoice's oscillators without the adsr (no per-sample loop)
//   comb_delay     feedback comb through a delay line (loop in sub-blocks of 119 frames)
//   loop_z1        one-pole feedback through z1 (per-sample loop, 3 ops)
//   zdf_tanh       nonlinear ZDF one-pole: per-sample solve (6 Newton iterations; tangents since T1b.1-fix1)
//   chain32_fast   chain32 with fast_ transcendentals (everything inline)
// T1b.1-fix1: the same at the default IR level with contraction opted in (`+contract`) and at machine-code levels
// 1–3 (`cgN`). Compile time: 100- and 1000-step mixed chains (element-wise, z1, a one-pole loop every 10 steps) at
// O1/O2/O3, and the 100-step chain and the M1 review's 1200-input mux at every machine-code level.
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq/jit/compiler.h"
#include "pq_bench.h"

using namespace pq;
namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr uint32_t kBlock = 64;
std::string gFilter;  // run only benchmarks whose name contains this (first argument; "--compile": compile times only)

g::Schedule scheduleOf(const std::string& text) {
  auto parsed = g::parse(text, "bench.pqp");
  if (!parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().message.c_str());
    std::exit(1);
  }
  g::PatchLibrary lib;
  lib.add(*parsed);
  g::InferOptions io;
  io.library = &lib;
  auto typed = g::infer(parsed->patches[0], io);
  if (!typed) std::exit(2);
  g::OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = g::optimise(std::move(typed.value()), oo);
  if (!opt) std::exit(3);
  auto s = g::schedule(opt.value());
  if (!s) {
    std::fprintf(stderr, "%s\n", s.error().message.c_str());
    std::exit(4);
  }
  return std::move(s.value());
}

// One compiler per machine-code level (CompilerConfig::codeGenLevel); `compiler()` is the default one.
jit::Compiler& compilerAt(uint32_t codeGenLevel) {
  static std::unique_ptr<jit::Compiler> c[4];
  if (!c[codeGenLevel]) {
    auto r = jit::Compiler::create({codeGenLevel});
    if (!r) {
      std::fprintf(stderr, "%s\n", r.error().message.c_str());
      std::exit(6);
    }
    c[codeGenLevel] = std::move(r.value());
  }
  return *c[codeGenLevel];
}
jit::Compiler& compiler() { return compilerAt(jit::CompilerConfig::kDefaultCodeGenLevel); }

// Runs blocks of kBlock frames with a sine input, `voices` voices (gates held, staggered pitches).
bench::Stats runImage(e::ProgramImage& img, uint32_t voices) {
  const uint32_t groups = voices ? (voices + 7) / 8 : 1;
  const e::ImageInterface& io = img.io();
  std::vector<std::vector<float>> in(io.globalIn.size(), std::vector<float>(kBlock)), out(io.globalOut.size(), std::vector<float>(kBlock));
  std::vector<const float*> inP;
  std::vector<float*> outP;
  for (auto& b : in) {
    for (uint32_t i = 0; i < kBlock; ++i) b[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
    inP.push_back(b.data());
  }
  for (auto& b : out) outP.push_back(b.data());
  std::vector<std::vector<std::vector<float>>> sig(groups, std::vector<std::vector<float>>(e::kVoiceSignalCount, std::vector<float>(kBlock * 8)));
  std::vector<std::vector<std::vector<float>>> vout(groups, std::vector<std::vector<float>>(io.voiceOut.size(), std::vector<float>(kBlock * 8)));
  std::vector<std::vector<const float*>> sigP(groups);
  std::vector<std::vector<float*>> voutP(groups);
  std::vector<e::VoiceGroupIO> gio(groups);
  for (uint32_t gi = 0; gi < groups; ++gi) {
    for (uint32_t f = 0; f < kBlock; ++f)
      for (uint32_t l = 0; l < 8; ++l) {
        const uint32_t v = gi * 8 + l;
        sig[gi][static_cast<size_t>(e::VoiceSignal::Hz)][f * 8 + l] = 110.0f * (1.0f + 0.1f * static_cast<float>(v));
        sig[gi][static_cast<size_t>(e::VoiceSignal::Gate)][f * 8 + l] = dsp::boolMask(v < voices);
        sig[gi][static_cast<size_t>(e::VoiceSignal::Velocity)][f * 8 + l] = 0.8f;
      }
    for (auto& b : sig[gi]) sigP[gi].push_back(b.data());
    for (auto& b : vout[gi]) voutP[gi].push_back(b.data());
    gio[gi].laneMask = 0xFF;
    gio[gi].signals = sigP[gi].data();
    gio[gi].out = voutP[gi].data();
    gio[gi].numOut = static_cast<uint32_t>(voutP[gi].size());
  }
  e::VoiceTable vt;
  vt.groups = gio.data();
  vt.numGroups = voices ? groups : 0;
  e::ProcessContext ctx;
  ctx.frames = kBlock;
  ctx.in = inP.data();
  ctx.numIn = static_cast<uint32_t>(inP.size());
  ctx.out = outP.data();
  ctx.numOut = static_cast<uint32_t>(outP.size());
  ctx.voices = voices ? &vt : nullptr;
  DenormalGuard dg;
  const bench::Stats s = bench::measure([&] { img.process(ctx); }, kBlock);
  if (!out.empty()) bench::doNotOptimize(out[0][kBlock - 1]);
  return s;
}

void runBench(const char* name, const std::string& text, uint32_t voices, uint32_t optLevel = 2, bool contract = false,
              uint32_t codeGenLevel = jit::CompilerConfig::kDefaultCodeGenLevel) {
  if (!gFilter.empty() && std::string(name).find(gFilter) == std::string::npos) return;
  const uint32_t groups = voices ? (voices + 7) / 8 : 1;
  const g::Schedule sched = scheduleOf(text);
  e::InterpretOptions eo;
  eo.maxBlockFrames = kBlock;
  eo.maxVoiceGroups = groups;
  auto interp = e::buildInterpreted(sched, eo);
  if (!interp) std::exit(5);
  jit::CompileOptions jo;
  jo.maxBlockFrames = kBlock;
  jo.maxVoiceGroups = groups;
  jo.optLevel = optLevel;
  jo.contract = contract;
  jit::CompileStats cs;
  auto img = compilerAt(codeGenLevel).compile(sched, jo, &cs);
  if (!img) {
    std::fprintf(stderr, "%s\n", img.error().message.c_str());
    std::exit(7);
  }
  const bench::Stats si = runImage(**interp, voices);
  const bench::Stats sj = runImage(**img, voices);
  const double per = voices > 1 ? static_cast<double>(voices) : 1.0;
  const char* unit = voices > 1 ? "ns/voice-sample" : "ns/sample";
  char label[128];
  std::snprintf(label, sizeof label, "interp/%s (%zu ops)", name, sched.ops.size());
  bench::report(label, {si.medianNs / per, si.minNs / per}, unit);
  const bool cgDefault = codeGenLevel == jit::CompilerConfig::kDefaultCodeGenLevel;
  std::snprintf(label, sizeof label, "jit-O%u%s%s%s/%s", optLevel, contract ? "+contract" : "", cgDefault ? "" : "-cg",
                cgDefault ? "" : std::to_string(codeGenLevel).c_str(), name);
  bench::report(label, {sj.medianNs / per, sj.minNs / per}, unit);
  std::printf("      speed-up %.1fx; compile %.1f ms (IR %.1f, opt %.1f, machine code %.1f); %u ops inline, %u called\n",
              si.medianNs / sj.medianNs, cs.totalMs, cs.codegenMs, cs.optimiseMs, cs.machineCodeMs, cs.inlinedOps, cs.calledOps);
}

std::string chain32(bool fast) {
  std::string t = "patch Chain {\n  in x : audio\n  out y : audio\n  a0 = x\n";
  const char* ops[] = {"$ * 0.99 + 0.01", fast ? "fast_tanh($)" : "tanh($)", "min($, 0.9)", "$ * $ - 0.1", "max($, -0.9)",
                       "abs($) * 0.5", "$ + 0.25", fast ? "fast_sin($)" : "sin($)"};
  for (int i = 1; i <= 32; ++i) {
    std::string ex = ops[(i - 1) % 8];
    const std::string prev = "a" + std::to_string(i - 1);
    for (size_t k; (k = ex.find('$')) != std::string::npos;) ex.replace(k, 1, prev);
    t += "  a" + std::to_string(i) + " = " + ex + "\n";
  }
  t += "  y = a32\n}\n";
  return t;
}

const char* kFmVoice = R"(
patch FmVoice {
  param ratio = 2.0 [0.25..16]
  param index = 1.0 [0..20]
  param attack  = 5ms   [0ms..10s]
  param release = 300ms [1ms..30s]
  out   out : audio voice mono
  mod = sine(phasor(voice.hz * ratio))
  car = sine(phasor(voice.hz + mod * index * voice.hz))
  env = adsr(voice.gate, a: attack, d: 200ms, s: 0.7, r: release)
  out = car * env
}
patch adsr {
  in    gate : bool audio voice mono
  param a = 5ms   [0ms..10s]
  param d = 200ms [1ms..10s]
  param s = 0.7   [0..1]
  param r = 300ms [1ms..30s]
  out   out : audio voice mono
  attacking = srff(rise(gate), level >= 0.999 || !gate)
  target    = attacking ? 1.0 : gate ? s : 0.0
  time      = attacking ? a : gate ? d : r
  coef      = 1 - exp(-1 / max(time * sr, 1))
  level     = z1(level + (target - level) * coef)
  out = level
}
)";

const char* kFmOsc = R"(patch FmOsc {
  param ratio = 2.0 [0.25..16]
  param index = 1.0 [0..20]
  out   out : audio voice mono
  mod = sine(phasor(voice.hz * ratio))
  car = sine(phasor(voice.hz + mod * index * voice.hz))
  out = car * gate_to_real(voice.gate)
})";

const char* kComb = R"(patch Comb {
  in x : audio
  param fb = 0.7 [0..0.95]
  out y : audio
  c = delay(x + c * fb, 2.5ms, 10ms)
  y = c
})";

const char* kLoopZ1 = R"(patch OnePole {
  in x : audio
  out y : audio
  lp = z1(lp + (x - lp) * 0.1)
  y = lp
})";

const char* kZdf = R"(patch TanhOnePole {
  in x : audio
  param cutoff = 3kHz [20Hz..20kHz]
  out y : audio
  g = tan(3.141592653589793 * cutoff / sr)
  s = z1(s2)
  solve(iterations: 6) {
    unknown v = s
    v == s + g * (tanh(x) - tanh(v))
  }
  s2 = 2 * v - s
  y = v
})";

std::string mixedChain(int n) {
  std::string t = "patch Chain {\n  in x : audio\n  param p = 0.5 [0..1]\n  out y : audio\n  a0 = x\n";
  for (int i = 1; i <= n; ++i) {
    const std::string prev = "a" + std::to_string(i - 1), cur = "a" + std::to_string(i);
    switch (i % 10) {
      case 0: t += "  " + cur + " = z1(" + cur + " + (" + prev + " - " + cur + ") * 0.1)\n"; break;
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
  }
  return t + "  y = a" + std::to_string(n) + "\n}\n";
}

// docs/reviews/M1/probes/mux1200.pqp
std::string bigMux(int n) {
  std::string t = "patch BigMux {\n  out y : audio\n  p = phasor(3Hz)\n  x = sine(phasor(220Hz))\n";
  std::string args;
  for (int i = 0; i < n; ++i) {
    t += "  d" + std::to_string(i) + " = x * " + std::to_string(1.0 - i / static_cast<double>(n)) + "\n";
    args += ", d" + std::to_string(i);
  }
  return t + "  y = mux(p * " + std::to_string(n) + args + ")\n}\n";
}

void compileTimeCg(const char* name, const std::string& text) {
  const g::Schedule s = scheduleOf(text);
  for (uint32_t cg = 0; cg <= 3; ++cg) {
    jit::CompileStats best;
    best.totalMs = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
      jit::CompileStats cs;
      auto img = compilerAt(cg).compile(s, {}, &cs);
      if (!img) std::exit(8);
      if (cs.totalMs < best.totalMs) best = cs;
    }
    std::printf("BENCH jit-compile/%s (%zu ops) cg%u   total %8.1f ms (machine code %.1f), %llu B code, stack probes %d\n", name,
                s.ops.size(), cg, best.totalMs, best.machineCodeMs, static_cast<unsigned long long>(best.objectBytes),
                best.stackProbes ? 1 : 0);
  }
}

void compileTime(int n) {
  const g::Schedule s = scheduleOf(mixedChain(n));
  for (uint32_t level : {1u, 2u, 3u}) {
    jit::CompileOptions jo;
    jo.optLevel = level;
    jit::CompileStats best;
    best.totalMs = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
      jit::CompileStats cs;
      auto img = compiler().compile(s, jo, &cs);
      if (!img) std::exit(8);
      if (cs.totalMs < best.totalMs) best = cs;
    }
    std::printf("BENCH jit-compile/%d-step chain (%zu ops) O%u   total %8.1f ms (host %.2f, IR %.1f, opt %.1f, machine code %.1f), %u IR instructions\n",
                n, s.ops.size(), level, best.totalMs, best.hostMs, best.codegenMs, best.optimiseMs, best.machineCodeMs,
                best.irInstructions);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const bool compileOnly = argc > 1 && std::string(argv[1]) == "--compile";
  if (argc > 1 && !compileOnly) gFilter = argv[1];
  std::printf("T1b.1 JIT vs interpreter, block %u frames, 48 kHz; JIT target %s\n", kBlock, compiler().hostCpu().c_str());
  if (!compileOnly) {
    for (uint32_t level : {0u, 1u, 2u, 3u}) {
      runBench("chain32", chain32(false), 0, level);
      runBench("chain32_fast", chain32(true), 0, level);
      runBench("fm_voice x8", kFmVoice, 8, level);
      runBench("fm_voice x64", kFmVoice, 64, level);
      runBench("fm_osc x8 (no envelope)", kFmOsc, 8, level);
      runBench("comb_delay", kComb, 0, level);
      runBench("loop_z1", kLoopZ1, 0, level);
      runBench("zdf_tanh", kZdf, 0, level);
    }
    // T1b.1-fix1: opt-in contraction, and the machine-code levels, at the default IR level.
    for (const auto& [name, text, voices] : {std::tuple<const char*, std::string, uint32_t>{"chain32", chain32(false), 0},
                                            {"chain32_fast", chain32(true), 0}, {"fm_voice x64", kFmVoice, 64},
                                            {"comb_delay", kComb, 0}, {"loop_z1", kLoopZ1, 0}, {"zdf_tanh", kZdf, 0}}) {
      runBench(name, text, voices, 1, true);
      for (uint32_t cg = 1; cg <= 3; ++cg)
        if (cg != jit::CompilerConfig::kDefaultCodeGenLevel) runBench(name, text, voices, 1, false, cg);
    }
  }
  if (gFilter.empty()) {
    compileTime(100);
    compileTime(1000);
    compileTimeCg("100-step chain", mixedChain(100));
    compileTimeCg("mux1200", bigMux(1200));
  }
  return 0;
}
