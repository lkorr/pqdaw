// T4.3: the stored layout (layout keys for anonymous nodes, lenient layout, spliceLayout: text → layout → text) and the
// automatic layout (layered placement + Manhattan channel routing; tidy, completing a partial layout, carrying the
// layout across a text edit).

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <chrono>
#include <cmath>
#include <random>
#include <string>

#include "pq/graph/layout.h"
#include "pq/graph/parse.h"
#include "text_util.h"

using namespace pq::graph;
namespace fs = std::filesystem;

namespace {

std::string diagnostics(const ParseResult& r, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : r.diagnostics) s += formatDiagnostic(src, d) + "\n";
  return s;
}

ParseResult parseOk(std::string_view src, ParseOptions opts = {}) {
  ParseResult r = parseDetailed(src, "layout_test.pqp", opts);
  REQUIRE_MESSAGE(r.ok(), diagnostics(r, src));
  return r;
}

struct Seg {
  Point2 a, b;
};

// The polyline of wire w: source pin, routing points, target pin.
std::vector<Point2> routeOf(const Patch& p, const LayoutGeometry& g, WireIndex w) {
  const Wire& wr = p.wire(w);
  const NodeLayout& a = p.layout(wr.from.node);
  const NodeLayout& b = p.layout(wr.to.node);
  std::vector<Point2> pts;
  pts.push_back({a.x + g.shape[wr.from.node].w, a.y + g.outPin(p, wr.from.node, wr.from.port)});
  for (const Point2& q : p.wireVia(w)) pts.push_back(q);
  pts.push_back({b.x, b.y + g.inPin(p, wr.to.node, wr.to.port)});
  return pts;
}

// Every node placed, no two nodes overlapping (with a gap), every wire orthogonal, and no wire segment through the
// interior of a node.
void checkLayout(const Patch& p, const LayoutGeometry& g, const std::string& what) {
  CAPTURE(what);
  const uint32_t n = p.nodeCount();
  for (NodeIndex v = 0; v < n; ++v) REQUIRE(p.layout(v).placed);
  for (NodeIndex a = 0; a < n; ++a)
    for (NodeIndex b = a + 1; b < n; ++b) {
      const NodeLayout &la = p.layout(a), &lb = p.layout(b);
      const bool overlap = la.x < lb.x + g.shape[b].w && lb.x < la.x + g.shape[a].w && la.y < lb.y + g.shape[b].h &&
                           lb.y < la.y + g.shape[a].h;
      CHECK_MESSAGE(!overlap, p.describe(a) << " overlaps " << p.describe(b));
    }
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    const auto pts = routeOf(p, g, w);
    for (size_t i = 1; i < pts.size(); ++i) {
      const Point2 a = pts[i - 1], b = pts[i];
      CHECK_MESSAGE((a.x == b.x || a.y == b.y), "wire " << w << " has a diagonal segment");
      // Interior of every node (shrunk by half a unit: pins sit on the edges).
      for (NodeIndex v = 0; v < n; ++v) {
        const NodeLayout& l = p.layout(v);
        const float x0 = l.x + 0.5f, x1 = l.x + g.shape[v].w - 0.5f, y0 = l.y + 0.5f, y1 = l.y + g.shape[v].h - 0.5f;
        const float sx0 = std::min(a.x, b.x), sx1 = std::max(a.x, b.x), sy0 = std::min(a.y, b.y), sy1 = std::max(a.y, b.y);
        const bool hits = sx0 < x1 && sx1 > x0 && sy0 < y1 && sy1 > y0;
        CHECK_MESSAGE(!hits, "wire " << w << " (" << p.describe(p.wire(w).from.node) << " -> " << p.describe(p.wire(w).to.node)
                                     << ") crosses " << p.describe(v));
      }
    }
  }
}

// A random patch of about `nodes` nodes: params and inputs, arithmetic over earlier values (fan-out included), a
// feedback loop through z1 every 25 nodes, outputs at the end.
Patch randomPatch(uint32_t nodes, uint32_t seed) {
  std::mt19937 rng(seed);
  Patch p("Random");
  std::vector<NodeIndex> values;
  for (uint32_t i = 0; i < 4; ++i) values.push_back(p.addInput("in" + std::to_string(i), portType(Kind::Real)));
  for (uint32_t i = 0; i < 6; ++i) values.push_back(p.addParam("p" + std::to_string(i), ParamDecl{}));
  const AtomId ops[] = {atomId("add"), atomId("mul"), atomId("sub"), atomId("max")};
  const AtomId z1 = atomId("z1"), tanh = atomId("tanh");
  while (p.nodeCount() + 4 < nodes) {
    const uint32_t r = rng() % 10;
    auto pick = [&] {  // mostly recent values (a deep graph), sometimes old ones (long wires)
      const size_t k = values.size();
      return (rng() % 4 == 0) ? values[rng() % k] : values[k - 1 - rng() % std::min<size_t>(k, 6)];
    };
    if (r < 7) {
      const NodeIndex a = p.addAtom(ops[rng() % 4]);
      p.connect(pick(), a, 0);
      p.connect(pick(), a, 1);
      values.push_back(a);
    } else if (r < 9) {
      const NodeIndex a = p.addAtom(tanh);
      p.connect(pick(), a, 0);
      values.push_back(a);
    } else {
      // y = z1(y * k + x): a delay loop
      const NodeIndex add = p.addAtom(atomId("add"));
      const NodeIndex d = p.addAtom(z1);
      const NodeIndex mul = p.addAtom(atomId("mul"));
      p.connect(add, d, 0);
      p.connect(d, mul, 0);
      p.connect(pick(), mul, 1);
      p.connect(mul, add, 0);
      p.connect(pick(), add, 1);
      values.push_back(d);
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    const NodeIndex o = p.addOutput("out" + std::to_string(i), portType(Kind::Real));
    p.connect(values[values.size() - 1 - i], o, 0);
  }
  return p;
}

}  // namespace

TEST_CASE("layout keys: anonymous nodes are placed by key and stay inline; print is stable") {
  ParseResult r = parseOk(R"(patch K {
  in x : audio
  param g = 0.5
  out y : audio
  y = tanh(x * g + 1) * 2
}
)");
  Patch& p = r.file.patches[0];
  // Place every node: the anonymous mul / tanh / add / consts get keys like `y.in.a`.
  for (NodeIndex v = 0; v < p.nodeCount(); ++v) {
    NodeLayout l;
    l.placed = true;
    l.x = static_cast<float>(10 * v);
    l.y = 1.5f;
    p.setLayout(v, l);
  }
  const std::string printed = print(r.file);
  CHECK(printed.find("  y = tanh(x * g + 1) * 2\n") != std::string::npos);  // nothing was named
  CHECK(printed.find("node y.in at (") != std::string::npos);               // the outer multiply
  CHECK(printed.find("node y.in.a.x.a at (") != std::string::npos);         // x * g + 1 → x * g
  CHECK(printed.find("node y.in.a.x.b at (") != std::string::npos);         // the literal 1
  const ParseResult again = parseOk(printed);
  CHECK(again.file == r.file);
  CHECK(print(again.file) == printed);
}

TEST_CASE("layout keys: routed wires between anonymous nodes; equations are `_e<k>`, made unique") {
  ParseResult r = parseOk(R"(patch E {
  in x : real
  out y : real
  _e0 = x * 2
  solve(iterations: 2) {
    unknown v = 0
    v * 3 == tanh(_e0 - v)
  }
  y = v + 1
}
)");
  Patch& p = r.file.patches[0];
  for (NodeIndex v = 0; v < p.nodeCount(); ++v) {
    NodeLayout l;
    l.placed = true;
    l.x = static_cast<float>(v);
    l.y = -static_cast<float>(v);
    p.setLayout(v, l);
  }
  for (WireIndex w = 0; w < p.wireCount(); ++w) p.setWireVia(w, {{static_cast<float>(w), 0.25f}});
  const std::string printed = print(r.file);
  // The user's node `_e0` keeps its name; the equation's key avoids it.
  CHECK(printed.find("node _e0_ at (") != std::string::npos);
  CHECK(printed.find("node _e0_.lhs at (") != std::string::npos);    // v * 3
  CHECK(printed.find("node _e0_.rhs.x at (") != std::string::npos);  // _e0 - v inside tanh
  CHECK(printed.find("wire _e0_.rhs.x.out -> _e0_.rhs.x via (") != std::string::npos);
  CHECK(printed.find("v * 3 == tanh(_e0 - v)") != std::string::npos);
  const ParseResult again = parseOk(printed);
  CHECK(again.file == r.file);
  CHECK(print(again.file) == printed);
}

TEST_CASE("layout: unknown keys are errors, or dropped and counted with lenientLayout") {
  const std::string src = R"(patch P {
  in x : real
  out y : real
  y = x * 2
  layout {
    node y.in at (1, 2)
    node y.in.b at (3, 4)
    node gone at (5, 6)
    node y.in.c at (7, 8)
    wire x.out -> y.in.q via ((1, 1))
  }
}
)";
  const ParseResult strict = parseDetailed(src, "p.pqp");
  REQUIRE(strict.diagnostics.size() == 3);
  CHECK(strict.diagnostics[0].message == "layout: unknown node 'gone'");
  CHECK(strict.diagnostics[1].message == "layout: 'mul' has no input 'c'");
  CHECK(strict.diagnostics[2].message == "layout: 'mul' has no input 'q'");
  ParseOptions lenient;
  lenient.lenientLayout = true;
  const ParseResult r = parseOk(src, lenient);
  CHECK(r.layoutDropped == 3);
  const Patch& p = r.file.patches[0];
  uint32_t placed = 0;
  for (NodeIndex v = 0; v < p.nodeCount(); ++v) placed += p.layout(v).placed ? 1u : 0u;
  CHECK(placed == 2);
}

TEST_CASE("spliceLayout: only the layout block changes; text -> layout -> text is byte-stable") {
  const std::string src = R"(// a comment that print() would drop
patch S {
  in x : audio      // aligned the author's way
  out y : audio
  y = tanh(x * 3)
}

patch T {
  out z : real
  z = 1
  layout {
    comment "old" at (0, 0)
  }
}
)";
  ParseResult r = parseOk(src);
  PatchFile laid = r.file;
  for (Patch& p : laid.patches) autoLayout(p, defaultGeometry(p));
  laid.patches[1].addComment({"kept", {1, 2}});
  auto spliced = spliceLayout(src, r, laid);
  REQUIRE(spliced.ok());
  const std::string& s1 = *spliced;
  CHECK(s1.find("// a comment that print() would drop") != std::string::npos);
  CHECK(s1.find("in x : audio      // aligned the author's way") != std::string::npos);
  CHECK(s1.find("    node y.in at (") != std::string::npos);
  const ParseResult r1 = parseOk(s1);
  CHECK(r1.file == laid);  // the layout reads back exactly
  auto s2 = spliceLayout(s1, r1, r1.file);
  REQUIRE(s2.ok());
  CHECK(*s2 == s1);  // byte-stable
  // Removing the placements gives the first patch's original text back (it had no layout block; the second keeps
  // its block for the comments, which the Patch API cannot remove).
  PatchFile bare = r1.file;
  for (Patch& p : bare.patches) {
    for (NodeIndex v = 0; v < p.nodeCount(); ++v) p.setLayout(v, NodeLayout{});
    for (WireIndex w = 0; w < p.wireCount(); ++w) p.setWireVia(w, {});
  }
  auto s3 = spliceLayout(s1, r1, bare);
  REQUIRE(s3.ok());
  CHECK(s3->find("node ") == std::string::npos);
  const std::string head = src.substr(0, src.find("patch T"));
  CHECK(s3->substr(0, head.size()) == head);
}

TEST_CASE("autoLayout: every corpus and stdlib patch is placed without overlaps and routed orthogonally") {
  size_t patches = 0;
  for (const fs::path& dir : {text_util::casesDir(), text_util::stdlibDir()}) {
    text_util::DirLoader loader(dir);
    for (const fs::path& path : text_util::pqpFiles(dir)) {
      const std::string src = text_util::readFile(path);
      ParseOptions opts;
      opts.imports = &loader;
      ParseResult r = parseDetailed(src, path.string(), opts);
      REQUIRE_MESSAGE(r.ok(), diagnostics(r, src));
      PatchFile laid = r.file;
      for (Patch& p : laid.patches) {
        const LayoutGeometry g = defaultGeometry(p);
        const AutoLayoutStats st = autoLayout(p, g);
        CHECK(st.crossings <= st.crossingsInitial);
        checkLayout(p, g, path.filename().string() + ":" + p.name());
        ++patches;
      }
      // text → layout → text: the laid-out file prints, parses back equal, and prints identically.
      const std::string printed = print(laid);
      const ParseResult back = parseDetailed(printed, path.string(), opts);
      REQUIRE_MESSAGE(back.ok(), diagnostics(back, printed));
      CHECK(back.file == laid);
      CHECK(print(back.file) == printed);
      // And spliced into the hand-written source.
      auto spliced = spliceLayout(src, r, laid);
      REQUIRE(spliced.ok());
      const ParseResult rs = parseDetailed(*spliced, path.string(), opts);
      REQUIRE_MESSAGE(rs.ok(), diagnostics(rs, *spliced));
      CHECK(rs.file == laid);
      auto again = spliceLayout(*spliced, rs, rs.file);
      REQUIRE(again.ok());
      CHECK(*again == *spliced);
    }
  }
  MESSAGE("laid out " << patches << " patches");
  CHECK(patches >= 30);
}

TEST_CASE("autoLayout: a feedback loop runs right to left through its own dummies; deterministic") {
  ParseResult r = parseOk(R"(patch Loop {
  in x : audio
  param k = 0.5
  out y : audio
  s = z1(x + s * k)
  y = s
}
)");
  Patch& p = r.file.patches[0];
  const LayoutGeometry g = defaultGeometry(p);
  const AutoLayoutStats st = autoLayout(p, g);
  CHECK(st.reversed == 1);
  checkLayout(p, g, "Loop");
  Patch q = r.file.patches[0];
  autoLayout(q, g);
  CHECK(q == p);
  // The feedback wire goes left: one wire has a routing point left of its source node.
  uint32_t leftward = 0;
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    bool left = false;
    for (const Point2& pt : p.wireVia(w)) left |= pt.x < p.layout(p.wire(w).from.node).x;
    leftward += left ? 1u : 0u;
  }
  CHECK(leftward == 1);
}

TEST_CASE("autoLayout: random 500-node patches are laid out cleanly") {
  for (uint32_t seed = 1; seed <= 3; ++seed) {
    Patch p = randomPatch(500, seed);
    REQUIRE(p.validate().ok());
    const LayoutGeometry g = defaultGeometry(p);
    const AutoLayoutStats st = autoLayout(p, g);
    CHECK(st.crossings <= st.crossingsInitial);
    MESSAGE("seed " << seed << ": " << st.layers << " layers, " << st.dummies << " dummies, " << st.reversed
                    << " feedback, crossings " << st.crossingsInitial << " -> " << st.crossings << ", " << st.tracks << " tracks");
    checkLayout(p, g, "random " + std::to_string(seed));
  }
}

TEST_CASE("autoLayout: routing a 500-node patch takes < 50 ms" * doctest::test_suite("perf")) {
  Patch p = randomPatch(500, 7);
  const LayoutGeometry g = defaultGeometry(p);
  double best = 1e9;
  for (int i = 0; i < 5; ++i) {
    Patch q = p;
    const auto t0 = std::chrono::steady_clock::now();
    autoLayout(q, g);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  MESSAGE("autoLayout 500 nodes: " << best << " ms");
  CHECK(best < 50.0);
}

TEST_CASE("completeLayout keeps placed nodes and places the rest without overlaps; routeUnrouted") {
  ParseResult r = parseOk(R"(patch Part {
  in x : audio
  param g = 0.5
  out y : audio
  m = x * g
  t = tanh(m)
  y = t
  layout {
    node m at (200, 40)
    node y at (400, 40)
  }
}
)");
  Patch& p = r.file.patches[0];
  const LayoutGeometry g = defaultGeometry(p);
  const NodeIndex m = *p.findByName("m");
  const uint32_t added = completeLayout(p, g);
  CHECK(added == p.nodeCount() - 2);
  CHECK(p.layout(m).x == 200.0f);
  CHECK(p.layout(m).y == 40.0f);
  CHECK(fullyPlaced(p));
  for (NodeIndex a = 0; a < p.nodeCount(); ++a)
    for (NodeIndex b = a + 1; b < p.nodeCount(); ++b) {
      const NodeLayout &la = p.layout(a), &lb = p.layout(b);
      CHECK_FALSE((la.x < lb.x + g.shape[b].w && lb.x < la.x + g.shape[a].w && la.y < lb.y + g.shape[b].h &&
                   lb.y < la.y + g.shape[a].h));
    }
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    const auto pts = routeOf(p, g, w);
    for (size_t i = 1; i < pts.size(); ++i) CHECK((pts[i - 1].x == pts[i].x || pts[i - 1].y == pts[i].y));
  }
}

TEST_CASE("carryLayout: positions survive a text edit through the inherited NodeIds") {
  const std::string before = R"(patch C {
  in x : audio
  out y : audio
  y = tanh(x * 3) + x
}
)";
  ParseResult r0 = parseOk(before);
  Patch& p0 = r0.file.patches[0];
  autoLayout(p0, defaultGeometry(p0));
  // The edited text (a literal changed, a new term) parsed with the previous parse: matched nodes keep their ids.
  const std::string after = R"(patch C {
  in x : audio
  out y : audio
  y = tanh(x * 4) + x + 1
}
)";
  ParseOptions o;
  o.previous = &r0.file;
  ParseResult r1 = parseOk(after, o);
  const uint32_t carried = carryLayout(r1.file, r0.file);
  Patch& p1 = r1.file.patches[0];
  CHECK(carried >= 4);  // x, y, tanh, the multiply at least
  const NodeIndex x0 = *p0.findByName("x"), x1 = *p1.findByName("x");
  CHECK(p1.layout(x1) == p0.layout(x0));
  CHECK_FALSE(fullyPlaced(p1));  // the new `+ 1` has no position yet
  completeLayout(p1, defaultGeometry(p1));
  CHECK(fullyPlaced(p1));
}
