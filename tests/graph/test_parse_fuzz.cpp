// T1.2 fuzz-ish tests: random token streams, random mutations of the valid corpus, and random graphs through the
// printer. Nothing may crash, hang or assert; whatever parses must round-trip.

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "pq/graph/parse.h"
#include "text_util.h"

using namespace pq::graph;

namespace {

// Checks the invariants of any parse: diagnostics are well-formed, and a successful parse round-trips.
void checkParse(std::string_view src, const ParseResult& r, int& parsed, ParseOptions opts = {}) {
  for (const Diagnostic& d : r.diagnostics) {
    CHECK(d.location.line >= 1);
    CHECK(d.location.col >= 1);
    CHECK(d.offset <= src.size());
    CHECK(!d.message.empty());
    (void)formatDiagnostic(src, d);
  }
  if (!r.ok()) return;
  ++parsed;
  const std::string printed = print(r.file);
  const ParseResult again = parseDetailed(printed, "fuzz.pqp", opts);
  REQUIRE_MESSAGE(again.ok(), "printed text does not parse:\n" << printed << "\nfrom:\n" << src);
  CHECK_MESSAGE(again.file == r.file, "round trip changed the file:\n" << src << "\n---\n" << printed);
  CHECK(print(again.file) == printed);
}

const char* const kVocabulary[] = {
    // keywords and contextual words
    "patch", "import", "param", "in", "out", "export", "solve", "iterations", "layout", "true", "false", "unknown",
    "node", "at", "rot", "flip", "wire", "via", "comment", "real", "bool", "audio", "const", "global", "voice",
    "mono", "stereo", "ch",
    // names and atoms
    "P", "Q", "x", "y", "z", "a", "b", "g", "sine", "phasor", "z1", "mux", "demux", "tanh", "adsr", "sr", "hz",
    "gate", "track", "lp", "pulse", "voice.hz", "transport.bpm", "track.in",
    // literals
    "0", "1", "2.5", "5ms", "1e3", "50%", "440Hz", "-3dB", "12st", "700c", "1e-3s", "64smp", "2kHz", "007",
    "\"s\"", "\"a\\\"b\"", "1e999", "5xs",
    // punctuation
    "{", "}", "(", ")", "[", "]", ",", ":", ";", "=", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%", "!",
    "&&", "||", "^^", "?", ".", "..", "@", "->",
    // junk
    "#", "&", "^", "\xC3\xA9", "/*", "*/", "//", "\"",
};

std::string randomStream(std::mt19937& rng) {
  std::uniform_int_distribution<size_t> pick(0, std::size(kVocabulary) - 1);
  std::uniform_int_distribution<int> len(0, 80), coin(0, 9);
  std::string s;
  const bool framed = coin(rng) < 6;  // most streams sit inside a patch, to get past the top level
  if (framed) s = coin(rng) < 5 ? "patch P {\n out y : real\n" : "patch P { ";
  const int n = len(rng);
  for (int i = 0; i < n; ++i) {
    s += kVocabulary[pick(rng)];
    s += coin(rng) < 2 ? "\n" : " ";
  }
  if (framed) s += "\n}";
  return s;
}

std::string mutate(const std::string& src, std::mt19937& rng) {
  std::string s = src;
  std::uniform_int_distribution<int> op(0, 5), count(1, 4);
  const int edits = count(rng);
  for (int e = 0; e < edits && !s.empty(); ++e) {
    std::uniform_int_distribution<size_t> pos(0, s.size() - 1);
    const size_t a = pos(rng);
    switch (op(rng)) {
      case 0: s.erase(a, std::min<size_t>(1 + rng() % 8, s.size() - a)); break;           // delete a few bytes
      case 1: s.insert(a, s.substr(pos(rng), 1 + rng() % 16)); break;                     // duplicate a range
      case 2: s.insert(a, std::string(" ") + kVocabulary[rng() % std::size(kVocabulary)] + " "); break;
      case 3: std::swap(s[a], s[pos(rng)]); break;                                         // swap two bytes
      case 4: s[a] = static_cast<char>(32 + rng() % 95); break;                            // replace a byte
      case 5: s.erase(a, std::min<size_t>(1 + rng() % 64, s.size() - a)); break;          // delete a line or so
    }
  }
  return s;
}

}  // namespace

TEST_CASE("fuzz: 10^4 random token streams") {
  std::mt19937 rng(12345);
  int parsed = 0;
  for (int i = 0; i < 10000; ++i) {
    const std::string src = randomStream(rng);
    CAPTURE(src);
    const ParseResult r = parseDetailed(src, "fuzz.pqp");
    checkParse(src, r, parsed);
  }
  MESSAGE(parsed << " of 10000 random streams parsed");
}

TEST_CASE("fuzz: random mutations of the valid corpus") {
  std::mt19937 rng(777);
  int parsed = 0, total = 0;
  text_util::DirLoader loader(text_util::casesDir());
  ParseOptions opts;
  opts.imports = &loader;
  for (const auto& path : text_util::pqpFiles(text_util::casesDir())) {
    const std::string original = text_util::readFile(path);
    for (int i = 0; i < 300; ++i) {
      const std::string src = mutate(original, rng);
      CAPTURE(src);
      const ParseResult r = parseDetailed(src, path.string(), opts);
      checkParse(src, r, parsed, opts);
      ++total;
    }
  }
  MESSAGE(parsed << " of " << total << " mutations parsed");
  CHECK(total >= 3000);
}

TEST_CASE("fuzz: the printer turns random graphs into text that parses back to the same shape") {
  std::mt19937 rng(4242);
  const char* const atoms[] = {"add", "sub", "mul", "div", "mod", "neg", "not", "and", "lt", "eq", "select",
                               "channel", "pack", "z1", "sine", "phasor", "tanh", "mux", "demux", "counter",
                               "voice.hz", "sr", "pulse", "noise", "clip", "lerp", "sampleRead"};
  int checked = 0;
  for (int iter = 0; iter < 2000; ++iter) {
    Patch p("R");
    const uint32_t params = rng() % 3, ins = rng() % 3, outs = 1 + rng() % 2;
    for (uint32_t i = 0; i < params; ++i) p.addParam("p" + std::to_string(i), ParamDecl{});
    for (uint32_t i = 0; i < ins; ++i) p.addInput("i" + std::to_string(i), portType(Kind::Real));
    const uint32_t numAtoms = rng() % 12;
    for (uint32_t i = 0; i < numAtoms; ++i) {
      const AtomId id = atomId(atoms[rng() % std::size(atoms)]);
      const AtomSignature& sig = atomSignature(id);
      // Variadic inputs take any count (one argument each); a variadic output count is not expressible in text v0
      // (QUESTIONS Q19), so those atoms keep the minimum.
      const bool variadic = sig.has(atom_flags::kVariadicInputs);
      const NodeIndex n = p.addAtom(id, variadic ? static_cast<uint16_t>(2 + rng() % 2) : 0);
      if (rng() % 4 == 0) p.setNodeName(n, "n" + std::to_string(i));
      if (rng() % 6 == 0) {
        NodeLayout l;
        l.placed = true;
        l.x = static_cast<float>(rng() % 1000) * 0.25f;
        l.y = -static_cast<float>(rng() % 100);
        p.setLayout(n, l);
      }
    }
    const uint32_t numConsts = rng() % 4;
    for (uint32_t i = 0; i < numConsts; ++i)
      p.addConst(Constant::real(static_cast<double>(static_cast<int>(rng() % 2001) - 1000) / 8.0,
                                static_cast<Unit>(rng() % 10)));
    for (uint32_t i = 0; i < outs; ++i) p.addOutput("o" + std::to_string(i), portType(Kind::Real));
    // Drive every input (required or not) from a random output: cycles and fan-out included.
    std::vector<PortRef> sources;
    for (NodeIndex n = 0; n < p.nodeCount(); ++n)
      for (uint16_t k = 0; k < p.node(n).numOutputs; ++k) sources.push_back({n, k});
    if (sources.empty()) continue;
    for (NodeIndex n = 0; n < p.nodeCount(); ++n)
      for (uint16_t k = 0; k < p.node(n).numInputs; ++k) {
        if (p.inputOptional(n, k) && rng() % 2) continue;
        const WireIndex w = p.connect(sources[rng() % sources.size()], PortRef{n, k});
        if (rng() % 8 == 0) p.setWireVia(w, {Point2{1.5f, 2.0f}, Point2{-3.0f, 4.25f}});
      }
    const std::string text = print(p);
    ParseOptions opts;
    opts.validate = false;  // random graphs mix kinds; this checks the printer's syntax, not the graph's types
    const ParseResult r = parseDetailed(text, "random.pqp", opts);
    REQUIRE_MESSAGE(r.ok(), text << "\n" << (r.ok() ? std::string() : formatDiagnostic(text, r.diagnostics[0])));
    const Patch& q = r.file.patches[0];
    CHECK_MESSAGE(q.nodeCount() == p.nodeCount(), text);
    CHECK_MESSAGE(q.wireCount() == p.wireCount(), text);
    CHECK_MESSAGE(print(q) == text, text);
    ++checked;
  }
  MESSAGE(checked << " random graphs printed and re-parsed");
}
