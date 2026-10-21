// T1.7: interpreted cost per sample of a few patches (the baseline the JIT, T1b, must beat). Release build only.
//   chain32        32-node global chain (mul/add/tanh/min…, audio rate), block 64
//   fm_voice ×8    PATCH_LANGUAGE §4 FmVoice (2 phasors, 2 sines, adsr from atoms), 8 voices = 1 group, block 64
//   fm_voice ×64   the same, 64 voices = 8 groups
//   fm_osc ×8      FmVoice's oscillators without the adsr (no per-sample loop)
//   comb_delay     feedback comb through a delay line (loop in sub-blocks of 119 frames), block 64
//   loop_z1        one-pole feedback through z1 (per-sample loop, 3 ops), block 64
//   zdf_tanh       nonlinear ZDF one-pole: per-sample solve (6 Newton iterations, central differences), block 64
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq_bench.h"

using namespace pq;
namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr uint32_t kBlock = 64;

struct Program {
  g::Schedule sched;
  std::unique_ptr<e::ProgramImage> image;
};

Program compile(const std::string& text, uint32_t groups) {
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
  Program p;
  p.sched = std::move(s.value());
  e::InterpretOptions eo;
  eo.maxBlockFrames = kBlock;
  eo.maxVoiceGroups = groups;
  auto img = e::buildInterpreted(p.sched, eo);
  if (!img) {
    std::fprintf(stderr, "%s\n", img.error().message.c_str());
    std::exit(5);
  }
  p.image = std::move(img.value());
  return p;
}

// Runs blocks of kBlock frames with a sine input, `voices` voices (gates held, staggered pitches).
void runBench(const char* name, const std::string& text, uint32_t voices) {
  const uint32_t groups = voices ? (voices + 7) / 8 : 1;
  Program p = compile(text, groups);
  e::ProgramImage& img = *p.image;
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
  char label[96];
  std::snprintf(label, sizeof label, "interp/%s (%zu ops)", name, p.sched.ops.size());
  bench::report(label, s, "ns/sample");
  if (voices > 1) {
    std::snprintf(label, sizeof label, "interp/%s per voice", name);
    bench::report(label, {s.medianNs / voices, s.minNs / voices}, "ns/voice-sample");
  }
  if (!out.empty()) bench::doNotOptimize(out[0][kBlock - 1]);
}

std::string chain32() {
  std::string t = "patch Chain {\n  in x : audio\n  out y : audio\n  a0 = x\n";
  const char* ops[] = {"$ * 0.99 + 0.01", "tanh($)", "min($, 0.9)", "$ * $ - 0.1", "max($, -0.9)", "abs($) * 0.5", "$ + 0.25", "sin($)"};
  for (int i = 1; i <= 32; ++i) {
    std::string e = ops[(i - 1) % 8];
    const std::string prev = "a" + std::to_string(i - 1);
    for (size_t k; (k = e.find('$')) != std::string::npos;) e.replace(k, 1, prev);
    t += "  a" + std::to_string(i) + " = " + e + "\n";
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

// The same oscillators without the envelope's per-sample loop.
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

}  // namespace

int main() {
  std::printf("T1.7 interpreter, block %u frames, 48 kHz\n", kBlock);
  runBench("chain32", chain32(), 0);
  runBench("fm_voice x8", kFmVoice, 8);
  runBench("fm_voice x64", kFmVoice, 64);
  runBench("fm_osc x8 (no envelope)", kFmOsc, 8);
  runBench("comb_delay", kComb, 0);
  runBench("loop_z1", kLoopZ1, 0);
  runBench("zdf_tanh", kZdf, 0);
  return 0;
}
