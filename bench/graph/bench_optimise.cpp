// Optimiser costs (T1.4). optimise() runs after inference on every structural edit, inside the 50–200 ms compile
// budget (SPEC §7.4). Measured on a ~1000-node flat patch built to exercise every pass (folding, identities, CSE,
// dead code, fusion), on the bench_infer chain (little to do: the cost of a sweep that finds nothing), and on the
// 16-instance synth of bench_infer (flattened subcircuits, per-instance hoisting).

#include <cstdio>
#include <string>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq_bench.h"

using namespace pq::graph;

namespace {

// Each step: a product folded by ×(1 + 0·3), a duplicate (CSE), a dead branch (DCE), a z1 feedback tap and an
// a·b + c that fuses. ~12 nodes per step.
std::string workText(int steps) {
  std::string s = "patch Bench {\n  in x : audio voice mono\n";
  for (int k = 0; k < 8; ++k) s += "  param p" + std::to_string(k) + " = 0.5 [0..1]\n";
  s += "  out y : audio voice mono\n";
  std::string prev = "x";
  for (int i = 0; i < steps; ++i) {
    const std::string n = std::to_string(i), p = "p" + std::to_string(i % 8);
    s += "  t" + n + " = fast_tanh(" + prev + " * " + p + " * (1 + 0 * 3))\n";
    s += "  u" + n + " = fast_tanh(" + p + " * " + prev + ")\n";
    s += "  dead" + n + " = sin(" + prev + ") * 3\n";
    s += "  a" + n + " = t" + n + " * (2 / 4) + z1(u" + n + ")\n";
    prev = "a" + n;
  }
  s += "  y = " + prev + "\n}\n";
  return s;
}

// The bench_parse / bench_infer chain (1010 nodes): in x; 8 params; 250 × { t = fast_tanh(prev · p[k]); a = t + z1(t) }.
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

bool bench(const char* name, const std::string& text) {
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
  OptimiseStats st;
  auto once = optimise(inf.typed, oo, &st);
  if (!once.ok()) {
    std::printf("%s: optimise failed: %s\n", name, once.error().message.c_str());
    return false;
  }
  std::printf("%s: %u → %u nodes in %u sweeps (folded %u, simplified %u, merged %u, removed %u, fused %u, "
              "rates lowered %u)\n",
              name, st.nodesBefore, st.nodesAfter, st.passes, st.folded, st.simplified, st.merged, st.removed,
              st.fused, st.ratesLowered);
  // optimise() takes its TypedPatch by value (it is consumed in the pipeline), so the copy is measured separately.
  const auto copy = pq::bench::measure(
      [&] {
        TypedPatch c = inf.typed;
        pq::bench::doNotOptimize(c);
      },
      1);
  const auto s = pq::bench::measure(
      [&] {
        auto r = optimise(inf.typed, oo);
        pq::bench::doNotOptimize(r);
      },
      1);
  pq::bench::report("  copy of the TypedPatch", copy, "ns");
  pq::bench::report("  copy + optimise", s, "ns");
  std::printf("  = %.1f ns per input node (optimise alone, copy subtracted)\n",
              (s.medianNs - copy.medianNs) / st.nodesBefore);
  return true;
}

}  // namespace

int main() {
  bool ok = bench("optimise: mixed work (~1000 nodes)", workText(52));
  ok &= bench("optimise: bench_infer chain (1010 nodes)", chainText(250));
  ok &= bench("optimise: 16 voice instances", instancesText(16));
  return ok ? 0 : 1;
}
