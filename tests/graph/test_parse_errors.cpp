// T1.2: error cases. Each case checks the first diagnostic's exact message and its line:col, which the table gives
// as a marker: the position of the marker's first occurrence in the source.

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <string>
#include <vector>

#include "pq/graph/parse.h"

using namespace pq::graph;

namespace {

struct ErrorCase {
  const char* source;
  const char* message;  // exact text of the first diagnostic
  const char* marker;   // the first diagnostic is at the first occurrence of this text
};

std::pair<uint32_t, uint32_t> lineCol(std::string_view src, size_t offset) {
  uint32_t line = 1, col = 1;
  for (size_t i = 0; i < offset; ++i) {
    if (src[i] == '\n') {
      ++line;
      col = 1;
    } else if ((static_cast<unsigned char>(src[i]) & 0xC0) != 0x80) {
      ++col;
    }
  }
  return {line, col};
}

const ErrorCase kCases[] = {
    // ---- lexical ----
    {"patch P { param x = 5xs }", "unknown unit suffix 'xs' (units: Hz kHz ms s smp dB c st %)", "xs"},
    {"patch P { y = \"abc }", "unterminated string", "\""},
    {"patch P { y = \"a\\nb\" }", "unknown escape in string (only \\\" and \\\\ are allowed)", "\\n"},
    {"/* never closed", "unterminated comment: '/*' has no matching '*/'", "/*"},
    {"patch P { y = a # b }", "unexpected character '#'", "#"},
    {"patch P { y = a & b }", "unexpected character '&' (did you mean '&&'?)", "&"},
    {"patch P { y = 1e999 }", "number '1e999' is out of range", "1e999"},
    {"patch P { y = \xC3\xA9 }", "unexpected character", "\xC3\xA9"},
    // ---- syntax ----
    {"foo", "expected 'patch' or 'import' at the top level, found 'foo'", "foo"},
    {"patch { }", "expected the patch name after 'patch', found '{'", "{"},
    {"patch in { }", "expected the patch name after 'patch', found 'in' (a keyword cannot be used as a name)", "in"},
    {"patch P { out y : real\n y = 1", "'{' of patch 'P' is never closed", "{"},
    {"patch P { y = 1 + }", "expected an expression, found '}'", "}"},
    {"patch P { out y : real\n y = (1 + 2 }", "expected ')' to close '(', found '}'", "}"},
    {"patch P { y : real }", "expected '=' after 'y', found ':' (declarations start with 'in', 'out' or 'param')", ":"},
    {"patch P { param x }", "expected '=' after the param name (a param needs a default: 'param x = 0.5'), found '}'", "}"},
    {"patch P { param x = y }",
     "a param default must be a constant: a number, true/false, or arithmetic (+ - * / %) on numbers", "y "},
    {"patch P { param x : bool = 1 }", "param 'x' is bool but its default is real", "1"},
    {"patch P { param x : audio = 1 }", "a param is a control value and cannot be 'audio'; declare it 'real'", "audio"},
    {"patch P { param x : bool = true [0..1] }", "only real params have a range", "["},
    {"patch P { param x = 1 [0, 1] }", "expected '..' between the range ends ([min..max]), found ','", ","},
    {"patch P { param x = 1 @label(foo()) }", "expected ',' between attribute arguments, found '('", "()"},
    {"patch P { in x : float }", "expected a value kind (real, bool or audio), found 'float'", "float"},
    {"patch P { in x : real voice audio }",
     "port type words must come in the order: kind, rate, poly, width (found 'audio')", "audio"},
    {"patch P { in x : audio audio }", "'audio' already means a real at audio rate; omit the rate", "audio }"},
    {"patch P { in x : real ch(0) }", "channel count must be 1..64", "0"},
    {"patch P { out y : real = 1 }", "an 'out' has no default; bind it with 'y = …'", "="},
    {"patch P { y = a < b < c }", "comparisons cannot be chained; use parentheses or '&&'", "< c"},
    {"patch P { y = --x }", "only one prefix operator is allowed; use parentheses, e.g. -(-x)", "-x"},
    {"patch P { y = phasor(hz: 1, 2) }", "positional arguments must come before named arguments", "2"},
    {"patch P { solve(iterations: 0) { unknown v\n v == 1 } }", "solve iterations must be 1..64 (found 0)", "0"},
    {"patch P { solve(iterations: 2.5) { } }", "expected the iteration count (an integer), found number '2.5'", "2.5"},
    {"patch P { solve(iterations: 2) { unknown v } }",
     "a solve block needs as many equations as unknowns (1 unknowns, 0 equations)", "solve"},
    {"patch P { solve(iterations: 2) { v + 1 } }", "expected 'unknown name' or an equation 'lhs == rhs' in a solve block",
     "+"},
    {"patch P { layout { node a at (0, 0) }\n y = 1 }", "the layout block must be the last item in a patch", "y ="},
    {"patch P { layout { box a } }", "expected 'node', 'wire' or 'comment' in a layout block, found 'box'", "box"},
    {"patch P { layout { node a at (0ms, 0) } }", "layout coordinates have no unit", "0ms"},
    {"patch P { layout { node a at (0, 0) rot 4.5 } }", "expected a rotation in degrees (an integer)", "4.5"},
    {"patch P { layout { wire a.out b.in } }", "expected '->' between the wire's ends, found 'b'", "b."},
    {"patch P { export = 1 }", "expected the export name, found '='", "="},
    {"patch P { param x = 1 @ }", "expected an attribute name after '@', found '}'", "}"},
    {"import foo", "expected string (the imported file) after 'import', found 'foo'", "foo"},
    {"import \"\"", "empty import path", "\"\""},
    {"patch P { patch Q { } }", "expected a declaration (param, in, out, export, solve, layout) or a binding 'name = …', "
                                "found 'patch' (missing '}' before it?)",
     "patch Q"},
    // ---- names and calls ----
    {"patch P { out y : real\n y = foo }", "unknown name 'foo'", "foo"},
    {"patch P { out y : real\n y = foo(1) }",
     "unknown function 'foo' (not a patch in this file, an imported patch or an atom)", "foo"},
    {"patch P { out y : real\n y = sine() }", "missing argument 'phase' in the call to 'sine'", "sine"},
    {"patch P { out y : real\n y = sine(1, 2) }", "too many arguments to 'sine' (2 given, it has 1 inputs)", "sine"},
    {"patch P { out y : real\n y = phasor(1, freq: 2) }", "'phasor' has no input 'freq' (inputs: hz, reset)", "freq"},
    {"patch P { out y : real\n y = phasor(1, hz: 2) }", "input 'hz' of 'phasor' is given twice", "hz:"},
    {"patch P { out y : real\n y = mux(1, 2) }", "'mux' needs at least 3 positional arguments", "mux"},
    {"patch P { out y : real\n x = 1\n x = 2\n y = x }", "'x' is already defined (at 2:2)", "x = 2"},
    {"patch P { out y : real\n y = 1\n y = 2 }", "output 'y' is bound twice (first at 2:2)", "y = 2"},
    {"patch P { param p = 1\n out y : real\n p = 2\n y = p }",
     "cannot bind 'p': it is a param (declared at 1:17)", "p = 2"},
    {"patch P { out y : real\n export e = 1\n e = 2\n y = 1 }",
     "export 'e' is bound in its declaration ('export e = …')", "e = 2"},
    {"patch P { out y : real\n a = b\n b = a\n y = a }", "names refer to each other in a cycle with no node: a = b = a",
     "a = b"},
    {"patch P { out y : real\n out z : real\n y = z\n z = 1 }",
     "'z' is an output and cannot be read; bind the value to a name and use that", "z\n"},
    {"patch P { out y : real\n y = voice.pitch }", "'voice.pitch' is not a built-in input", "voice"},
    {"patch P { out y : real\n y = voice }", "'voice' is a namespace; use one of its inputs, e.g. voice.hz", "voice"},
    {"patch P { out y : real\n sr = 1\n y = sr }",
     "'sr' is a built-in name (voice.*, transport.*, track.in, sidechain.in, sr) and cannot be redefined", "sr"},
    {"patch P { out y : real\n y = phasor(1).saw }", "'phasor' has no output 'saw' (outputs: out)", "saw"},
    {"patch P { out y : real\n y = send(1, 2) }", "'send' has no output to use as a value", "send"},
    {"patch P { param p = 1\n out y : real\n p.x = 1\n y = p }",
     "only inputs of a computed node can be bound with 'p.x = …'", "p.x"},
    {"patch P { in x : real\n out y : real\n h = samplehold(x, x > 0)\n h.trig = x > 1\n y = h }",
     "input 'trig' of 'h' is already connected", "trig ="},
    {"patch P {}\npatch P { }", "patch 'P' is defined twice", "P { }"},
    {"patch sine { }", "patch 'sine' has the name of a built-in atom", "sine"},
    {"patch P { out y : real\n y = 1\n layout { node q at (0, 0) } }", "layout: unknown node 'q'", "q"},
    {"patch P { in x : real\n out y : real\n y = x\n layout { node x at (0, 0)\n node x at (1, 1) } }",
     "layout: node 'x' is placed twice", "x at (1"},
    {"patch P { in x : real\n out y : real\n y = x\n layout { wire x.out -> y.x via ((1, 1)) } }",
     "layout: 'y' has no input 'x'", "x via"},
    {"patch P { in x : real\n out y : real\n out z : real\n y = x\n z = x\n layout { wire x.out -> q.in } }",
     "layout: unknown node 'q'", "q.in"},
    // ---- validation (T1.1), reported at the node's source position ----
    {"patch P { param p = 5 [0..1]\n out y : real\n y = p }",
     "patch 'P': param 'p' (param, #0): default is outside the range", "p ="},
    {"patch P { out y : real }", "patch 'P': dangling port: input 'in' of 'y' (out, #0) is not connected", "y"},
    {"patch A { out y : real\n y = 1 + B().o }\npatch B { out o : real\n o = A() }",
     "patch 'A' instantiates itself (through 'B')", "A {"},
};

}  // namespace

TEST_CASE("parse errors: each case reports its message at file:line:col") {
  int count = 0;
  for (const ErrorCase& c : kCases) {
    const std::string src = c.source;
    const std::string message = c.message;
    const size_t at = src.find(c.marker);
    REQUIRE_MESSAGE(at != std::string::npos, "marker not found: " << c.marker);
    CAPTURE(src);
    const ParseResult r = parseDetailed(src, "test.pqp");
    REQUIRE_FALSE(r.ok());
    const Diagnostic& d = r.diagnostics[0];
    CHECK(d.message == message);
    const auto [line, col] = lineCol(src, at);
    CHECK(d.location.file == "test.pqp");
    CHECK(d.location.line == line);
    CHECK(d.location.col == col);
    CHECK(d.offset == at);
    // The Result form carries the same location and the formatted text.
    const auto res = parse(src, "test.pqp");
    REQUIRE_FALSE(res.ok());
    CHECK(res.error().location.line == line);
    CHECK(res.error().message.find("test.pqp:" + std::to_string(line) + ":" + std::to_string(col) + ": error: " +
                                   message) == 0);
    ++count;
  }
  CHECK(count >= 30);
  MESSAGE(count << " error cases");
}

TEST_CASE("parse errors: formatted diagnostic with source line and caret") {
  const std::string src = "patch P {\n\tparam x = 5xs\n}";
  const auto r = parse(src, "units.pqp");
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == pq::ErrorCode::Parse);
  CHECK(r.error().message ==
        "units.pqp:2:13: error: unknown unit suffix 'xs' (units: Hz kHz ms s smp dB c st %)\n"
        "\tparam x = 5xs\n"
        "\t           ^~");
  // Columns count code points, and the caret line skips a multi-byte character as one column.
  const std::string utf = "patch P { /* \xC3\xA9 */ y = 1 + ? }";
  const ParseResult u = parseDetailed(utf, "u.pqp");
  REQUIRE_FALSE(u.ok());
  CHECK(u.diagnostics[0].location.col == 27);
  CHECK(formatDiagnostic(utf, u.diagnostics[0]) ==
        "u.pqp:1:27: error: expected an expression, found '?'\n" + utf + "\n" + std::string(26, ' ') + "^");
  // A diagnostic at the end of the file.
  const ParseResult e = parseDetailed("patch P {", "e.pqp");
  REQUIRE_FALSE(e.ok());
  CHECK(formatDiagnostic("patch P {", e.diagnostics[0]) ==
        "e.pqp:1:9: error: '{' of patch 'P' is never closed\npatch P {\n        ^");
}

TEST_CASE("parse errors: declared-type errors from validation have code Type") {
  const std::string src = "patch P { out y : real\n y = voice.gate * 2 }";
  const auto r = parse(src, "t.pqp");
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == pq::ErrorCode::Type);
  CHECK(r.error().message.find("t.pqp:2:6: error: patch 'P': kind mismatch: bool into real") == 0);
  CHECK(r.error().message.find("\n y = voice.gate * 2 }\n     ^~~~~~~~~~") != std::string::npos);
}

TEST_CASE("parse errors: recovery reports several problems in one pass") {
  const std::string src =
      "patch P {\n"
      "  param a = ;\n"        // 1: missing default
      "  in x : bogus\n"       // 2: bad kind
      "  out y : real\n"
      "  y = x +* 2\n"         // 3: bad expression
      "  solve(iterations: 1) {\n"
      "    unknown v\n"
      "    v + 1\n"            // 4: not an equation
      "    v == 2\n"
      "  }\n"
      "}\n"
      "patch Q { out z : real\n z = 1 }\n"
      "nonsense\n"             // 5: top level
      "patch R { out w : real\n w = ) }\n";  // 6
  const ParseResult r = parseDetailed(src, "multi.pqp");
  REQUIRE(r.diagnostics.size() == 6);
  CHECK(r.diagnostics[0].location.line == 2);
  CHECK(r.diagnostics[0].message == "expected an expression, found ';'");
  CHECK(r.diagnostics[1].location.line == 3);
  CHECK(r.diagnostics[1].message == "expected a value kind (real, bool or audio), found 'bogus'");
  CHECK(r.diagnostics[2].location.line == 5);
  CHECK(r.diagnostics[2].message == "expected an expression, found '*'");
  CHECK(r.diagnostics[3].location.line == 8);
  CHECK(r.diagnostics[4].location.line == 14);
  CHECK(r.diagnostics[5].location.line == 16);
  // Semantic errors are collected across the file too (lowering runs only when the syntax is valid).
  const ParseResult s = parseDetailed("patch P { out y : real\n y = a + b }\npatch Q { out z : real\n z = c }", "s.pqp");
  REQUIRE(s.diagnostics.size() == 3);
  CHECK(s.diagnostics[0].message == "unknown name 'a'");
  CHECK(s.diagnostics[1].message == "unknown name 'b'");
  CHECK(s.diagnostics[2].message == "unknown name 'c'");
}

TEST_CASE("parse errors: deep nesting is an error, not a stack overflow") {
  for (const char* open : {"(", "-(", "f(", "[", "!(", "a ? ("}) {
    std::string src = "patch P { out y : real\n y = ";
    for (int i = 0; i < 100000; ++i) src += open;
    const ParseResult r = parseDetailed(src, "deep.pqp");
    REQUIRE_FALSE(r.ok());
    CAPTURE(open);
    const std::string& m = r.diagnostics[0].message;
    const bool expected = m.find("nested too deeply") != std::string::npos || m.find("too deep") != std::string::npos ||
                          m.find("expected an expression") != std::string::npos;
    CHECK_MESSAGE(expected, m);
  }
  // A flat sum past kMaxExprDepth terms.
  std::string src = "patch P { out y : real\n y = 1";
  for (uint32_t i = 0; i < kMaxExprDepth; ++i) src += " + 1";
  src += " }";
  const ParseResult r = parseDetailed(src, "deep.pqp");
  REQUIRE_FALSE(r.ok());
  CHECK(r.diagnostics[0].message == "expression is too deep (more than " + std::to_string(kMaxExprDepth) +
                                        " levels); split it with named intermediate bindings");
}

TEST_CASE("parse errors: the diagnostic count is capped") {
  std::string src;
  for (int i = 0; i < 500; ++i) src += "#\n";
  const ParseResult r = parseDetailed(src, "cap.pqp");
  CHECK(r.diagnostics.size() == kMaxDiagnostics);
}
