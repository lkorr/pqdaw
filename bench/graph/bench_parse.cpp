// Patch-language costs on a ~1000-node patch (T1.2): lex+parse+lower, + validation, and printing. Parsing is on the
// compile path (full compile budget 50–200 ms, SPEC §7.4) and on every text edit of a patch. T1.x-text-ids: the
// NodeId inheritance from the previous parse (id_match.h), which also runs on every text edit, on the same chain
// and on the M1 review mux (1200 products: the corpus largest patch, 2409 nodes).

#include <cctype>
#include <cstdio>
#include <string>

#include "pq/graph/id_match.h"
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

// docs/reviews/M1/probes/mux1200.pqp: 1200 named products of one sine into one mux.
std::string muxText(int n) {
  std::string t = "patch BigMux {\n  out y : audio\n  p = phasor(3Hz)\n  x = sine(phasor(220Hz))\n";
  std::string args;
  for (int i = 0; i < n; ++i) {
    t += "  d" + std::to_string(i) + " = x * " + std::to_string(1.0 - i / static_cast<double>(n)) + "\n";
    args += ", d" + std::to_string(i);
  }
  return t + "  y = mux(p * " + std::to_string(n) + args + ")\n}\n";
}

// The export-on-top edit of T1.x-text-ids: one line after `patch … {` (renumbers every parse id).
std::string withExport(const std::string& text) {
  const size_t eol = text.find('\n', text.find("patch "));
  return text.substr(0, eol + 1) + "  export edit_spare = sine(phasor(331))\n" + text.substr(eol + 1);
}

// Renames every binding `<prefix><digits>` to `r<prefix><digits>` (names give no help: only structure pairs them).
std::string renamed(const std::string& text, char prefix) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool wordStart = i == 0 || !(std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_');
    if (text[i] == prefix && wordStart && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))
      out += 'r';
    out += text[i];
  }
  return out;
}

void benchInherit(const char* label, const std::string& before, const std::string& after) {
  const ParseResult a = parseDetailed(before, "bench.pqp");
  const ParseResult b = parseDetailed(after, "bench.pqp");
  if (!a.ok() || !b.ok()) {
    std::printf("%s: benchmark text does not parse\n", label);
    return;
  }
  IdMatchStats st;
  const auto inherit = pq::bench::measure(
      [&] {
        PatchFile f = b.file;  // the copy is measured separately and subtracted
        st = inheritNodeIds(f, a.file);
        pq::bench::doNotOptimize(f);
      },
      1);
  const auto copyOnly = pq::bench::measure(
      [&] {
        PatchFile f = b.file;
        pq::bench::doNotOptimize(f);
      },
      1);
  ParseOptions withPrev;
  withPrev.previous = &a.file;
  const auto reparse = pq::bench::measure(
      [&] {
        ParseResult r = parseDetailed(after, "bench.pqp", withPrev);
        pq::bench::doNotOptimize(r);
      },
      1);
  const double nodes = b.file.patches[0].nodeCount();
  std::printf("%s: %u nodes, %u wires; kept %u of %u (by name %u, structure %u, hash %u, weak %u)\n", label,
              b.file.patches[0].nodeCount(), b.file.patches[0].wireCount(), st.matched, st.previousNodes, st.byName,
              st.byStructure, st.byHash, st.weak);
  pq::bench::Stats net = inherit;
  net.medianNs -= copyOnly.medianNs;
  net.minNs -= copyOnly.minNs;
  pq::bench::report((std::string("inheritNodeIds (net of a file copy), ") + label).c_str(), net, "ns/file");
  std::printf("  = %.1f ns/node (the copy alone %.0f ns)\n", net.medianNs / nodes, copyOnly.medianNs);
  pq::bench::report((std::string("parse + validate + inherit, ") + label).c_str(), reparse, "ns/file");
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

  benchInherit("1010-node chain, export on top", text, withExport(text));
  benchInherit("1010-node chain, every binding renamed", text, renamed(renamed(text, 't'), 'a'));
  const std::string mux = muxText(1200);
  benchInherit("mux1200, export on top", mux, withExport(mux));
  benchInherit("mux1200, every product renamed", mux, renamed(mux, 'd'));

  // Round trip sanity (not timed).
  const ParseResult again = parseDetailed(print(ref.file), "bench.pqp");
  if (!again.ok() || !(again.file == ref.file)) {
    std::printf("round trip failed\n");
    return 1;
  }
  return 0;
}
