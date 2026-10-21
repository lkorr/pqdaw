// Patch-language costs on a ~1000-node patch (T1.2): lex+parse+lower, + validation, and printing. Parsing is on the
// compile path (full compile budget 50–200 ms, SPEC §7.4) and on every text edit of a patch.

#include <cstdio>
#include <string>

#include "pq/graph/parse.h"
#include "pq_bench.h"

using namespace pq::graph;

namespace {

// The same graph as bench_patch.cpp's chain (1010 nodes, 1501 wires) written as text: in x; 8 params with a range and
// a label; out y; 250 steps of  t = fast_tanh(prev · p[k]),  a = t + z1(t). Every 10th step is placed in the layout.
std::string chainText(int steps) {
  std::string s = "// generated benchmark patch\npatch Bench {\n  in x : audio voice mono\n";
  for (int k = 0; k < 8; ++k) s += "  param p" + std::to_string(k) + " = 0.5 [0..1] @label(\"Gain\")\n";
  s += "  out y : audio voice mono\n\n";
  std::string prev = "x";
  for (int i = 0; i < steps; ++i) {
    const std::string t = "t" + std::to_string(i), a = "a" + std::to_string(i);
    s += "  " + t + " = fast_tanh(" + prev + " * p" + std::to_string(i % 8) + ")\n";
    s += "  " + a + " = " + t + " + z1(" + t + ")\n";
    prev = a;
  }
  s += "  y = " + prev + "\n\n  layout {\n";
  for (int i = 0; i < steps; i += 10) s += "    node a" + std::to_string(i) + " at (" + std::to_string(i) + ", 10)\n";
  s += "  }\n}\n";
  return s;
}

}  // namespace

int main() {
  const std::string text = chainText(250);
  ParseOptions noValidate;
  noValidate.validate = false;
  const ParseResult ref = parseDetailed(text, "bench.pqp");
  if (!ref.ok()) {
    std::printf("benchmark patch does not parse: %s\n", ref.diagnostics[0].message.c_str());
    return 1;
  }
  const Patch& p = ref.file.patches[0];
  std::printf("patch: %u nodes, %u wires, %zu bytes of text\n", p.nodeCount(), p.wireCount(), text.size());
  const double nodes = p.nodeCount();

  const auto parseOnly = pq::bench::measure(
      [&] {
        ParseResult r = parseDetailed(text, "bench.pqp", noValidate);
        pq::bench::doNotOptimize(r);
      },
      1);
  pq::bench::report("parse (lex+syntax+lower), 1010 nodes", parseOnly, "ns/file");
  std::printf("  = %.1f ns/node\n", parseOnly.medianNs / nodes);

  const auto parseValidate = pq::bench::measure(
      [&] {
        ParseResult r = parseDetailed(text, "bench.pqp");
        pq::bench::doNotOptimize(r);
      },
      1);
  pq::bench::report("parse + validate, 1010 nodes", parseValidate, "ns/file");
  std::printf("  = %.1f ns/node, %.1f MB/s\n", parseValidate.medianNs / nodes,
              static_cast<double>(text.size()) / parseValidate.medianNs * 1e3);

  const auto printed = pq::bench::measure(
      [&] {
        std::string s = print(ref.file);
        pq::bench::doNotOptimize(s);
      },
      1);
  pq::bench::report("print, 1010 nodes", printed, "ns/file");
  std::printf("  = %.1f ns/node\n", printed.medianNs / nodes);

  // Round trip sanity (not timed).
  const ParseResult again = parseDetailed(print(ref.file), "bench.pqp");
  if (!again.ok() || !(again.file == ref.file)) {
    std::printf("round trip failed\n");
    return 1;
  }
  return 0;
}
