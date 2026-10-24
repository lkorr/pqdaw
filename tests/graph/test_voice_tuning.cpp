// T2.6: tune_voices — infer() rebinds voice.hz / voice.cents inside the subcircuits a patch instantiates to the
// patch's tuning node, leaves the binding patch's own reads (the fallback) on the engine's values, and drops the
// binder node.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"

using namespace pq::graph;

namespace {

struct Run {
  ParseResult parsed;
  PatchLibrary lib;
  InferResult inferred;
};

void infer(Run& r, std::string_view src) {
  r.parsed = parseDetailed(src, "test.pqp");
  REQUIRE(r.parsed.ok());
  r.lib.add(r.parsed.file);
  InferOptions o;
  o.library = &r.lib;
  r.inferred = inferDetailed(r.parsed.file.patches[0], o);
}

// "atom:port" of what drives input `input` of the node named `name` in the flattened patch ("const", "-": none).
std::string driver(const Patch& p, std::string_view name, uint32_t input = 0) {
  const auto n = p.findByName(name);
  if (!n) return "<no node>";
  const Adjacency adj = p.adjacency();
  const WireIndex w = adj.driver[p.flatInput(*n, input)];
  if (w == kNone) return "-";
  const PortRef from = p.wire(w).from;
  const Node& src = p.node(from.node);
  if (src.kind != NodeKind::Atom) return std::string(toText(src.kind));
  return std::string(atomSignature(src.atom).name) + ":" + std::to_string(from.port);
}

bool hasAtom(const Patch& p, std::string_view atom) {
  for (const Node& n : p.nodes())
    if (n.kind == NodeKind::Atom && atomSignature(n.atom).name == atom) return true;
  return false;
}

constexpr const char* kTuned = R"(
patch Track {
  param gen = 700 [0..1200]
  out f : audio voice
  out c : audio voice
  out own : audio voice
  t = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, gen, table: 0)
  tv = tune_voices(t.hz, t.cents)
  inst = Synth()
  f = inst.f
  c = inst.c
  own = voice.hz
}
patch Synth {
  out f : audio voice
  out c : audio voice
  x = voice.hz * 1
  y = voice.cents * 1
  f = x
  c = y
}
)";

}  // namespace

TEST_CASE("voice tuning: callee voice.hz / voice.cents read the tuning node, the binding patch reads the engine") {
  Run r;
  infer(r, kTuned);
  REQUIRE(r.inferred.ok());
  const Patch& p = r.inferred.typed.patch;
  // Inside Synth: voice.hz * 1 → its `a` input comes from tuning_hz output 0, voice.cents from output 1.
  CHECK(driver(p, "inst__x", 0) == "tuning_hz:0");
  CHECK(driver(p, "inst__y", 0) == "tuning_hz:1");
  // The root's own read and the node's fallback inputs stay the engine's.
  CHECK(driver(p, "own") == "voice.hz:0");
  CHECK(driver(p, "t", 2) == "voice.hz:0");
  CHECK(driver(p, "t", 3) == "voice.cents:0");
  CHECK_FALSE(hasAtom(p, "tune_voices"));  // consumed
  // Types follow the tuning node (audio voice).
  const auto f = p.findByName("inst__x");
  REQUIRE(f);
  CHECK(toText(r.inferred.typed.outputType(*f, 0)) == "real audio voice mono");
}

TEST_CASE("voice tuning: without `cents` connected voice.cents stays the engine's; nested binders; errors") {
  {
    Run r;
    infer(r, R"(
patch Track {
  out f : audio voice
  out c : audio voice
  t = tuning_hz(voice.degree, voice.equave, voice.hz, table: 0)
  tv = tune_voices(t.hz)
  inst = Synth()
  f = inst.f
  c = inst.c
}
patch Synth {
  out f : audio voice
  out c : audio voice
  x = voice.hz * 1
  y = voice.cents * 1
  f = x
  c = y
}
)");
    REQUIRE(r.inferred.ok());
    CHECK(driver(r.inferred.typed.patch, "inst__x", 0) == "tuning_hz:0");
    CHECK(driver(r.inferred.typed.patch, "inst__y", 0) == "voice.cents:0");
  }
  {
    // A rack binds a second tuning inside an instrument that is itself under the track's binding: the inner binder
    // wins for its callees, and its own fallback input reads the OUTER tuning (not the engine).
    Run r;
    infer(r, R"(
patch Track {
  out f : audio voice
  out g : audio voice
  t = tuning_hz(voice.degree, voice.equave, voice.hz, table: 0)
  tv = tune_voices(t.hz)
  rack = Rack()
  f = rack.f
  g = rack.g
}
patch Rack {
  out f : audio voice
  out g : audio voice
  u = tuning_hz(voice.degree, voice.equave, voice.hz * 2, table: 1)
  tv = tune_voices(u.hz)
  s = Synth()
  f = s.f
  gg = voice.hz * 3
  g = gg
}
patch Synth {
  out f : audio voice
  x = voice.hz * 1
  f = x
}
)");
    REQUIRE(r.inferred.ok());
    const Patch& p = r.inferred.typed.patch;
    CHECK(driver(p, "rack__s__x", 0) == "tuning_hz:0");
    CHECK(driver(p, "rack__gg", 0) == "tuning_hz:0");  // Rack's own voice.hz: the Track's tuning
    // Rack's tuning node: its fallback is (Track's tuned hz) · 2, and the Synth reads Rack's node, not Track's.
    const auto u = p.findByName("rack__u");
    const auto t = p.findByName("t");
    REQUIRE(u);
    REQUIRE(t);
    const Adjacency adj = p.adjacency();
    const PortRef synthIn = p.wire(adj.driver[p.flatInput(*p.findByName("rack__s__x"), 0)]).from;
    CHECK(synthIn.node == *u);
    const PortRef rackOwn = p.wire(adj.driver[p.flatInput(*p.findByName("rack__gg"), 0)]).from;
    CHECK(rackOwn.node == *t);
  }
  {
    Run r;
    infer(r, R"(
patch Track {
  out f : audio voice
  a = tune_voices(voice.hz * 2)
  b = tune_voices(voice.hz * 3)
  f = voice.hz
}
)");
    REQUIRE_FALSE(r.inferred.ok());
    CHECK(r.inferred.diagnostics.front().message.find("only once") != std::string::npos);
  }
  {
    // A voice binding needs a voice context, like voice.* itself.
    Run r;
    r.parsed = parseDetailed("patch Fx {\n  out y : audio\n  a = tune_voices(440)\n  y = 0\n}\n", "test.pqp");
    REQUIRE(r.parsed.ok());
    InferOptions o;
    o.voiceContext = false;
    r.inferred = inferDetailed(r.parsed.file.patches[0], o);
    CHECK_FALSE(r.inferred.ok());
  }
}

TEST_CASE("voice tuning: the text round-trips, and optimise() drops the rebound engine reads") {
  ParseResult pr = parseDetailed(kTuned, "test.pqp");
  REQUIRE(pr.ok());
  const std::string printed = print(pr.file);
  ParseResult again = parseDetailed(printed, "test.pqp");
  REQUIRE(again.ok());
  CHECK(again.file == pr.file);
  Run r;
  infer(r, kTuned);
  REQUIRE(r.inferred.ok());
  auto opt = optimise(std::move(r.inferred.typed), OptimiseOptions{});
  REQUIRE(opt.ok());
  // The callee's voice.hz / voice.cents reads are dead after the rebinding: only the root's remain (the node's
  // fallback inputs and `own`, possibly merged by CSE), and they feed only the tuning node and `own`.
  const Patch& q = opt.value().patch;
  const Adjacency adj = q.adjacency();
  int hz = 0, cents = 0;
  for (NodeIndex n = 0; n < q.nodeCount(); ++n) {
    if (q.node(n).kind != NodeKind::Atom) continue;
    const std::string_view a = atomSignature(q.node(n).atom).name;
    if (a != "voice.hz" && a != "voice.cents") continue;
    (a == "voice.hz" ? hz : cents)++;
    for (const WireIndex w : adj.outgoing(n)) {
      const Node& to = q.node(q.wire(w).to.node);
      const bool ok =
          to.kind == NodeKind::Output || (to.kind == NodeKind::Atom && atomSignature(to.atom).name == "tuning_hz");
      CHECK(ok);
    }
  }
  CHECK(hz >= 1);
  CHECK(hz <= 2);
  CHECK(cents == 1);
}
