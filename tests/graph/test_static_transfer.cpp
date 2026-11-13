// DY.3: graph::StaticTransfer (pq/graph/response.h): the static curve of a memoryless circuit equals its scalar formula
// exactly (the runtime's kernels); a stateful atom on the path gives no curve; an LFO on a control gives none unless the
// control is a set port; a param change re-runs only the ops that depend on the input and equals a fresh prepare;
// evaluate() does not allocate.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq/graph/response.h"

using namespace pq::graph;

namespace {

constexpr double kSr = 48000.0;

TypedPatch compile(const std::string& text) {
  ParseResult pr = parseDetailed(text, "transfer.pqp");
  if (!pr.ok()) FAIL(formatDiagnostic(text, pr.diagnostics.front()));
  PatchLibrary lib;
  lib.add(pr.file);
  InferOptions io;
  io.library = &lib;
  io.voiceContext = false;
  auto inf = infer(pr.file.patches[0], io);
  if (!inf.ok()) FAIL(inf.error().message);
  return std::move(*inf);
}

NodeIndex named(const Patch& p, std::string_view name) {
  const auto n = p.findByName(name);
  REQUIRE_MESSAGE(n.has_value(), std::string(name));
  return *n;
}
ResponseEnd inEnd(const TypedPatch& t, std::string_view name) { return {named(t.patch, name), 0, false}; }
ResponseEnd outEnd(const TypedPatch& t, std::string_view name) { return {named(t.patch, name), 0, true}; }

std::vector<float> grid() {
  std::vector<float> xs;
  for (int i = -40; i <= 40; ++i) xs.push_back(0.1f * static_cast<float>(i));
  return xs;
}

// The Saturator's "Hard Curve" (pq/document modules.cpp), after a drive gain: v / (1 + v^4)^(1/4), in float, op for op.
float hardCurve(float x, float drive) {
  const float v = x * drive;
  const float v2 = v * v;
  const float v4 = v2 * v2;
  return v / std::sqrt(std::sqrt(1.0f + v4));
}

const char* kShaper = R"(patch W {
  in x : audio global mono
  param drive = 2 [0..10]
  out y : audio
  v = x * drive
  v2 = v * v
  y = v / sqrt(sqrt(1 + v2 * v2))
})";

}  // namespace

TEST_CASE("static transfer: a memoryless waveshaper equals its scalar formula exactly; a param change re-runs only the "
          "ops that depend on the input and equals a fresh prepare; evaluate() does not allocate") {
  const TypedPatch t = compile(kShaper);
  StaticTransfer st;
  st.prepare(t, inEnd(t, "x"), outEnd(t, "y"));
  REQUIRE(st.status() == ResponseKind::Exact);
  REQUIRE(st.paramNodes().size() == 1);
  CHECK(st.sinkChannels() == 1);
  const std::vector<float> xs = grid();
  std::vector<float> ys(xs.size());
  REQUIRE(st.evaluate(xs, kSr, ys) == ResponseKind::Exact);
  for (size_t k = 0; k < xs.size(); ++k) CHECK_MESSAGE(ys[k] == hardCurve(xs[k], 2.0f), "x " << xs[k]);
  // The param and the constants run once per evaluate; per point only what reads the input (x itself, v, v², v⁴,
  // 1 + v⁴, the roots, the division).
  CHECK(st.perPointOps() == 8);
  st.setParam(0, 5.0f);
  CHECK(pq::alloc_trap::countRtViolations([&] { (void)st.evaluate(xs, kSr, ys); }) == 0);
  REQUIRE(st.evaluate(xs, kSr, ys) == ResponseKind::Exact);
  for (size_t k = 0; k < xs.size(); ++k) CHECK(ys[k] == hardCurve(xs[k], 5.0f));
  StaticTransfer fresh;
  fresh.prepare(t, inEnd(t, "x"), outEnd(t, "y"));
  fresh.setParam(0, 5.0f);
  std::vector<float> ys2(xs.size());
  REQUIRE(fresh.evaluate(xs, kSr, ys2) == ResponseKind::Exact);
  CHECK(ys == ys2);
  // CPU (rough, the check build): the cost of one curve at the display's 128 points.
  std::vector<float> xs128(128), ys128(128);
  for (size_t k = 0; k < xs128.size(); ++k) xs128[k] = -4.0f + 8.0f * static_cast<float>(k) / 127.0f;
  const auto t0 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < 20; ++rep) (void)st.evaluate(xs128, kSr, ys128);
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 20.0;
  MESSAGE("static transfer, 8 ops per point, 128 points: " << us << " us per evaluate");
}

TEST_CASE("static transfer: a z1 between In and Out gives no curve; a stateful atom only beside the path does not "
          "matter; a sink the input does not reach is a constant curve; source == sink is the identity") {
  {
    const TypedPatch t = compile(R"(patch Z {
  in x : audio global mono
  out y : audio
  y = x + 0.5 * z1(x)
})");
    StaticTransfer st;
    st.prepare(t, inEnd(t, "x"), outEnd(t, "y"));
    CHECK(st.status() == ResponseKind::Unavailable);
    const std::vector<float> xs = grid();
    std::vector<float> ys(xs.size());
    CHECK(st.evaluate(xs, kSr, ys) == ResponseKind::Unavailable);
    CHECK(std::isnan(ys[0]));
  }
  {
    // A constant: the curve is flat whatever x.
    const TypedPatch t = compile(R"(patch K {
  in x : audio global mono
  param g = 0.25 [0..1]
  out y : audio
  out z : audio
  y = g * 2
  z = x
})");
    StaticTransfer st;
    st.prepare(t, inEnd(t, "x"), outEnd(t, "y"));
    REQUIRE(st.status() == ResponseKind::Exact);
    const float xs[] = {-1.0f, 0.0f, 3.0f};
    float ys[3] = {};
    REQUIRE(st.evaluate(xs, kSr, ys) == ResponseKind::Exact);
    for (const float y : ys) CHECK(y == 0.5f);
    StaticTransfer id;
    id.prepare(t, inEnd(t, "x"), outEnd(t, "z"));
    REQUIRE(id.evaluate(xs, kSr, ys) == ResponseKind::Exact);
    for (int k = 0; k < 3; ++k) CHECK(ys[k] == xs[k]);
  }
}

TEST_CASE("static transfer: a control an LFO drives has no curve unless it is a set port; held at a value, the curve is "
          "the circuit with that value") {
  const TypedPatch t = compile(R"(patch L {
  in x : audio global mono
  out y : audio
  g = 1 + 0.5 * sine(phasor(2Hz))
  y = clip(x * g, -1, 1)
})");
  const std::vector<float> xs = grid();
  std::vector<float> ys(xs.size());
  {
    StaticTransfer plain;
    plain.prepare(t, inEnd(t, "x"), outEnd(t, "y"));
    CHECK(plain.status() == ResponseKind::Exact);  // (the path is memoryless; the LFO is only an unknown control)
    CHECK(plain.evaluate(xs, kSr, ys) == ResponseKind::Unavailable);
  }
  const uint32_t port = t.patch.flatOutput(named(t.patch, "g"), 0);
  StaticTransfer st;
  st.prepare(t, inEnd(t, "x"), outEnd(t, "y"), std::span<const uint32_t>(&port, 1));
  REQUIRE(st.setPorts().size() == 1);
  CHECK(st.evaluate(xs, kSr, ys) == ResponseKind::Unavailable);  // no value given yet
  st.setValue(0, 1.5f);
  REQUIRE(st.evaluate(xs, kSr, ys) == ResponseKind::Exact);
  for (size_t k = 0; k < xs.size(); ++k) CHECK(ys[k] == std::clamp(xs[k] * 1.5f, -1.0f, 1.0f));
}
