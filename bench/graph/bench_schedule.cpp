// Scheduler costs (T1.5). schedule() runs after optimise() on every structural edit, inside the 50–200 ms compile
// budget (SPEC §7.4), and also feeds the interpreter that plays while the JIT compiles, so it should stay well under
// a millisecond for patch sizes users build. Measured on optimised patches of ~1000 nodes:
//   - a chain (bench_infer's: 250 × { t = fast_tanh(prev · p); a = t + z1(t) }, per voice): one block region,
//   - feedback: 160 one-pole/comb sections (z1 and delay-line loops: 320 loop regions with split kernels),
//   - the bench_infer 16-instance synth (flattened subcircuits: voice stage, reducer, broadcasts, SVF loops),
//   - the same chain rescheduled with the previous state layout (the hot-swap path, T1.9).

#include <cstdio>
#include <string>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq_bench.h"

using namespace pq::graph;

namespace {

std::string chainText(int steps) {
  std::string s = "patch Bench {\n  in x : audio voice mono\n";
  for (int k = 0; k < 8; ++k) s += "  param p" + std::to_string(k) + " = 0.5 [0..1]\n";
  s += "  out y : audio voice mono\n";
  std::string prev = "x";
  for (int i = 0; i < steps; ++i) {
    const std::string t = "t" + std::to_string(i), a = "a" + std::to_string(i);
    s += "  " + t + " = fast_tanh(" + prev + " * p" + std::to_string(i % 8) + ")\n";
    s += "  " + a + " = " + t + " + z1(" + t + ")\n";
    prev = a;
  }
  s += "  y = " + prev + "\n}\n";
  return s;
}

// Each section: a one-pole loop (z1, per sample) and a comb (delay line, sub-blocks).
std::string feedbackText(int sections) {
  std::string s = "patch Bench {\n  in x : audio\n  param fb = 0.5 [0..0.9]\n  out y : audio\n";
  std::string prev = "x";
  for (int i = 0; i < sections; ++i) {
    const std::string n = std::to_string(i);
    s += "  lp" + n + " = z1(lp" + n + " + (" + prev + " - lp" + n + ") * 0.3)\n";
    s += "  c" + n + " = delay(lp" + n + " + c" + n + " * fb, " + std::to_string(20 + i % 50) + "smp, 100smp)\n";
    prev = "c" + n;
  }
  s += "  y = " + prev + "\n}\n";
  return s;
}

// bench_infer's synth: 16 calls of a 2-op FM voice with an SVF-like filter, half of them with a modulated cutoff.
std::string instancesText(int calls) {
  std::string s = "patch Bench {\n  param cutoff = 1000 [20..20000]\n  param res = 0.5 [0..1]\n  out y : audio global stereo\n";
  std::string sum = "0";
  for (int i = 0; i < calls; ++i) {
    const std::string v = "v" + std::to_string(i);
    if (i % 2) s += "  " + v + " = Voice(voice.hz * " + std::to_string(i + 1) + ", cutoff: cutoff + sine(phasor(3Hz)) * 100, res: res)\n";
    else s += "  " + v + " = Voice(voice.hz * " + std::to_string(i + 1) + ", cutoff: cutoff, res: res)\n";
    sum = "(" + sum + " + " + v + ")";
  }
  s += "  y = (" + sum + ", " + sum + ")\n}\n";
  s += "patch Voice {\n  in hz : audio voice\n  param cutoff = 1000 [20..20000]\n  param res = 0.5 [0..1]\n  out y : audio\n"
       "  mod = sine(phasor(hz * 2))\n  car = sine(phasor(hz + mod * 3 * hz))\n  f = Svf(car, cutoff: cutoff, res: res)\n"
       "  y = f * gate_to_real(voice.gate)\n}\n";
  s += "patch Svf {\n  in x : audio\n  param cutoff = 1000 [20..20000]\n  param res = 0.5 [0..1]\n  out lp : audio\n"
       "  g = tan(3.14159 * cutoff / sr)\n  k = 2 - 2 * res\n  a1 = 1 / (1 + g * (g + k))\n  a2 = g * a1\n  a3 = g * a2\n"
       "  v3 = x - ic2\n  v1 = a1 * ic1 + a2 * v3\n  v2 = ic2 + a2 * ic1 + a3 * v3\n"
       "  ic1 = z1(2 * v1 - ic1)\n  ic2 = z1(2 * v2 - ic2)\n"
       "  s1 = v2 * 0.5 + v1 * 0.25\n  s2 = s1 * s1 + v3\n  s3 = clip(s2, -1, 1) + abs(v1) * 0.01\n  s4 = s3 - floor(s3) + frac(v2) * 0.001\n"
       "  s5 = max(min(s4, 1), -1) + lerp(v1, v2, 0.5) * 0.001\n  s6 = s5 * g + v1 * k + v2 * a1 + v3 * a2 + s4 * a3\n"
       "  lp = v2 + s6 * 0.0001\n}\n";
  return s;
}

bool bench(const char* name, const std::string& text, bool withPrevious = false) {
  const ParseResult parsed = parseDetailed(text, "bench.pqp");
  if (!parsed.ok()) {
    std::printf("%s: does not parse: %s\n", name, parsed.diagnostics[0].message.c_str());
    return false;
  }
  PatchLibrary lib;
  lib.add(parsed.file);
  InferOptions io;
  io.library = &lib;
  const InferResult inf = inferDetailed(parsed.file.patches[0], io);
  if (!inf.ok()) {
    std::printf("%s: does not infer: %s\n", name, inf.diagnostics[0].message.c_str());
    return false;
  }
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(inf.typed, oo);
  if (!opt.ok()) {
    std::printf("%s: optimise failed: %s\n", name, opt.error().message.c_str());
    return false;
  }
  const TypedPatch& tp = opt.value();
  ScheduleOptions so;
  const ScheduleResult first = scheduleDetailed(tp, so);
  if (!first.ok()) {
    std::printf("%s: schedule failed: %s\n", name, first.diagnostics[0].message.c_str());
    return false;
  }
  if (withPrevious) so.previous = &first.schedule.state;
  const Schedule& s = first.schedule;
  uint32_t loops = 0;
  for (const Region& r : s.regions) loops += r.kind == RegionKind::Loop;
  std::printf("%s: %u nodes → %zu ops, %zu regions (%u loops), %zu stages, %u block + %u pattern buffers, "
              "%zu state entries (%u B global, %u B per voice group)\n",
              name, tp.patch.nodeCount(), s.ops.size(), s.regions.size(), loops, s.stages.size(), s.blockBuffers(),
              s.patternBuffers(), s.state.entries.size(), s.state.globalBytes, s.state.voiceBytes);
  const auto st = pq::bench::measure(
      [&] {
        auto r = scheduleDetailed(tp, so);
        pq::bench::doNotOptimize(r);
      },
      1);
  pq::bench::report(withPrevious ? "  schedule (previous layout)" : "  schedule", st, "ns");
  std::printf("  = %.1f ns per node\n", st.medianNs / tp.patch.nodeCount());
  return true;
}

}  // namespace

int main() {
  bool ok = bench("schedule: bench_infer chain (1010 nodes)", chainText(250));
  ok &= bench("schedule: bench_infer chain, previous layout", chainText(250), true);
  ok &= bench("schedule: 160 feedback sections", feedbackText(160));
  ok &= bench("schedule: 16 voice instances", instancesText(16));
  return ok ? 0 : 1;
}
