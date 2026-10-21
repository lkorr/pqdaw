// Type inference costs (T1.3). Inference reruns on every structural edit, inside the 50–200 ms compile budget
// (SPEC §7.4), so it should stay a small fraction of it: measured on a 1000-node flat patch, a 1000-node patch made
// of subcircuit instances, and the deepest allowed subcircuit nesting.

#include <cstdio>
#include <string>

#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq_bench.h"

using namespace pq::graph;

namespace {

// The bench_parse chain (1010 nodes, 1501 wires): in x; 8 params; 250 × { t = fast_tanh(prev · p[k]); a = t + z1(t) }.
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

// A synth-like patch: 8 voices' worth of a 2-op FM voice + SVF-like filter subcircuit, each instance ~60 nodes,
// called 16 times with different modulation (half the filter cutoffs modulated), summed into a global output.
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

// Nesting depth `depth`: P0 calls P1 … calls P{depth}; each level has a few nodes of its own.
std::string nestedText(uint32_t depth) {
  std::string s;
  for (uint32_t i = 0; i <= depth; ++i) {
    s += "patch P" + std::to_string(i) + " {\n  in x : audio\n  param g = 0.5 [0..1]\n  out y : audio\n";
    if (i < depth) s += "  c = P" + std::to_string(i + 1) + "(x * g, g: g * 0.5)\n  y = tanh(c) + z1(c) * g\n";
    else s += "  y = x * g\n";
    s += "}\n";
  }
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
  InferOptions opts;
  opts.library = &lib;
  const Patch& root = parsed.file.patches[0];
  const InferResult ref = inferDetailed(root, opts);
  if (!ref.ok()) {
    std::printf("%s: does not infer: %s\n", name, ref.diagnostics[0].message.c_str());
    return false;
  }
  const auto& tp = ref.typed;
  std::printf("%s: root %u nodes → flat %u nodes, %u wires, %zu instances\n", name, root.nodeCount(),
              tp.patch.nodeCount(), tp.patch.wireCount(), tp.instances.size());
  const auto s = pq::bench::measure(
      [&] {
        InferResult r = inferDetailed(root, opts);
        pq::bench::doNotOptimize(r);
      },
      1);
  pq::bench::report(name, s, "ns/infer");
  std::printf("  = %.1f ns per flat node\n", s.medianNs / tp.patch.nodeCount());
  return true;
}

}  // namespace

int main() {
  bool ok = bench("infer: flat chain (1010 nodes)", chainText(250));
  ok &= bench("infer: 16 voice instances (2 levels)", instancesText(16));
  ok &= bench("infer: nesting depth 64", nestedText(kMaxSubcircuitDepth));
  return ok ? 0 : 1;
}
