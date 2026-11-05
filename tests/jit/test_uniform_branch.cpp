// TX.uniform-branch: selects of block-uniform conditions branch once per block (pq/graph/schedule.h "Uniform
// branches") on both backends. Every run below is compared four ways, bit for bit: the interpreter and the JIT, each
// on the schedule with its branches and on the same schedule without them (both sides of every select computed: what
// the program computed before TX.uniform-branch, op for op and buffer for buffer). Params step every few blocks, so
// conditions flip between blocks and a side skipped in one block is needed in the next; voice conditions differ per
// lane (mixed, all, none, per group and per pair of groups). No allocation on the audio thread (RtScope).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace fs = std::filesystem;
namespace jit = pq::jit;

namespace {

jit::Compiler& branchCompiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    if (!r) {
      std::fprintf(stderr, "%s\n", r.error().message.c_str());
      std::abort();
    }
    return std::move(r.value());
  }();
  return *c;
}

ImageBuilder jitWith(jit::VoiceCode vc) {
  return [vc](const Schedule& s, const eng::InterpretOptions& io) {
    jit::CompileOptions o;
    o.maxBlockFrames = io.maxBlockFrames;
    o.maxVoiceGroups = io.maxVoiceGroups;
    o.resources = io.resources;
    o.sleep = io.sleep;
    o.voiceCode = vc;
    return branchCompiler().compile(s, o);
  };
}

// Params jump every `every` blocks (bools toggle); a quarter of the steps keep the value.
float stepped(uint32_t slot, const ParamSlot& p, uint64_t t0, float base, uint32_t every) {
  const uint64_t step = t0 / (64u * every);
  if (step == 0) return base;
  uint64_t h = (step * 0x9E3779B97F4A7C15ull) ^ (uint64_t{slot + 1} * 0xBF58476D1CE4E5B9ull);
  h ^= h >> 31;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 29;
  if ((h & 3u) == 0) return base;
  if (p.kind == Kind::Bool) return (h >> 8) & 1u ? 1.0f : 0.0f;
  const double u = static_cast<double>((h >> 11) & 0xFFFFF) / static_cast<double>(0xFFFFF);
  if (p.hasRange) return static_cast<float>(p.rangeMin + u * (p.rangeMax - p.rangeMin));
  const double b = std::isfinite(base) ? base : p.defaultValue;
  return static_cast<float>(b * (0.5 + u));
}

uint64_t bitDiffs(const Streams& a, const Streams& b, std::string& where) {
  uint64_t n = 0;
  if (a.size() != b.size()) return UINT64_MAX;
  for (const auto& [key, x] : a) {
    const auto it = b.find(key);
    if (it == b.end() || it->second.size() != x.size()) return UINT64_MAX;
    for (size_t t = 0; t < x.size(); ++t) {
      const double p = x[t], q = it->second[t];
      if (std::isnan(p) && std::isnan(q)) continue;
      if (std::memcmp(&p, &q, sizeof p) != 0) {
        if (n == 0) where = "stream " + std::to_string(key.kind) + "/" + std::to_string(key.a) + " voice " +
                            std::to_string(key.v) + " t=" + std::to_string(t);
        ++n;
      }
    }
  }
  return n;
}

uint32_t voicesOf(const Compiled& c) {
  for (const Stage& s : c.sched.stages)
    if (s.poly == Poly::Voice) return 11;  // two groups, the second one partly active
  return 0;
}

// The four runs of `c` (interpreter / JIT × with / without branches), compared bit for bit.
struct Four {
  uint32_t runs = 0;
  bool resources = false;  // needs tables the harness does not have
};
Four fourWays(const Compiled& c, uint32_t frames, uint32_t block, uint32_t every, jit::VoiceCode vc = jit::VoiceCode::Auto) {
  Four res;
  Compiled plain = c;  // ScheduleOptions::uniformBranches = false: the same schedule without the two tables
  plain.sched.branches.clear();
  plain.sched.guardOf.clear();
  Setup with(c, voicesOf(c), 0), without(plain, voicesOf(plain), 0);
  auto motion = [every](uint32_t slot, const ParamSlot& p, uint64_t t0, float base) {
    return stepped(slot, p, t0, base, every);
  };
  with.paramAt = motion;
  without.paramAt = motion;
  const InterpRun ia = runInterp(with, frames, block, 1024);
  if (!ia.error.empty() && ia.error.find("resource") != std::string::npos) {
    res.resources = true;
    return res;
  }
  REQUIRE_MESSAGE(ia.error.empty(), ia.error);
  const InterpRun ib = runInterp(without, frames, block, 1024);
  const InterpRun ja = runInterp(with, frames, block, 1024, jitWith(vc));
  const InterpRun jb = runInterp(without, frames, block, 1024, jitWith(vc));
  REQUIRE_MESSAGE(ib.error.empty(), ib.error);
  REQUIRE_MESSAGE(ja.error.empty(), ja.error);
  REQUIRE_MESSAGE(jb.error.empty(), jb.error);
  CHECK(ia.rtViolations == 0);
  CHECK(ja.rtViolations == 0);
  std::string where;
  CHECK_MESSAGE(bitDiffs(ib.streams, ia.streams, where) == 0, "interp: branches change the output at " << where);
  CHECK_MESSAGE(bitDiffs(ib.streams, ja.streams, where) == 0, "JIT with branches differs from the interpreter at " << where);
  CHECK_MESSAGE(bitDiffs(ib.streams, jb.streams, where) == 0, "JIT without branches differs from the interpreter at " << where);
  res.runs = 4;
  return res;
}

std::vector<fs::path> corpusFiles() {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(patchesDir())) files.push_back(p);
  for (const fs::path& p : text_util::pqpFiles(fs::path(PQ_TEST_DATA_DIR) / "jit" / "patches")) files.push_back(p);
  for (const fs::path& p : text_util::pqpFiles(fs::path(PQ_TEST_DATA_DIR) / "golden" / "patches")) files.push_back(p);
  const fs::path stdlib = fs::path(PQ_TEST_DATA_DIR).parent_path() / "stdlib";
  for (const auto& e : fs::recursive_directory_iterator(stdlib))
    if (e.path().extension() == ".pqp") files.push_back(e.path());
  return files;
}

constexpr const char* kUnits[] = {
    // one select of a param condition
    R"(patch One {
  in x : audio
  param p = 0.7 [0..1]
  out y : audio
  y = select(p > 0.5, tanh(x * 3) * 0.5, x * x - 0.25)
}
)",
    // a value both sides read and one an output reads too
    R"(patch Shared {
  in x : audio
  param p = 0.7 [0..1]
  out y : audio
  out z : audio
  s = x * 0.7 + 0.1
  a = tanh(s * 2)
  y = select(p > 0.5, a * 0.5, s * s)
  z = a
}
)",
    // nested selects, the inner one's sides sharing a value
    R"(patch Nested {
  in x : audio
  param p = 0.7 [0..1]
  param q = 0.2 [0..1]
  out y : audio
  t = x * 2 - 0.3
  inner = select(q > 0.5, sin(t) * 0.25 + t * t * t, t * t)
  y = select(p > 0.5, inner + 0.5, sin(x * 3) * 0.3 + x * x * 0.1 - 0.2)
}
)",
    // stateful ops on the sides (a one-pole, a delay, a phasor) keep their state whichever side runs
    R"(patch Stateful {
  in x : audio
  param p = 0.7 [0..1]
  out y : audio
  lp = z1(lp + (x * 2 - lp) * 0.1)
  e = delay(x * 0.5, 7smp, 16smp)
  osc = sine(phasor(x * 100 + 300))
  y = select(p > 0.5, tanh(lp * 3) * 0.5 + e * (lp + 0.5) - lp * lp, osc * 0.2 - 0.1 + osc * osc * x)
}
)",
    // a condition computed from params (the VCA's `warped`), one side the raw input
    R"(patch Control {
  in x : audio
  param p = 0.7 [0..1]
  param q = 0.2 [0..1]
  out y : audio
  w = abs(p - 0.5) + q > 0.7
  y = select(w, clip(x * 4 - 1, -1, 1) * 0.5 + x * x * 0.25 - abs(x) * 0.1, x)
}
)",
    // a bool param (toggled by the motion) and a select chain of one condition
    R"(patch Bool {
  in x : audio
  param on : bool = true
  out y : audio
  a = select(on, tanh(x * 2) * 0.7 + x * x, x * 0.5)
  y = select(on, a * a - 0.1 + sin(a), a)
}
)",
    // per voice: velocity against a param threshold (mixed lanes, all, none; per group and per pair)
    R"(patch Voice {
  param p = 0.5 [0..1]
  out y : audio voice
  osc = sine(phasor(voice.hz))
  y = select(voice.velocity > 0.4 + p * 0.6, tanh(osc * 3) * 0.5 + osc * osc * 0.2 - 0.1, osc * 0.25)
}
)",
    // per voice, nested, with a global param condition outside and a voice one inside
    R"(patch VoiceNested {
  param p = 0.5 [0..1]
  param q = 0.5 [0..1]
  out y : audio voice
  osc = sine(phasor(voice.hz * 2))
  inner = select(voice.velocity > 0.3 + q * 0.6, osc * osc * osc + 0.1 * osc, tanh(osc) * 0.5 + osc * 0.1)
  y = select(p > 0.5, inner * 0.8 + osc * 0.05, osc * 0.3 - osc * osc * 0.1)
}
)",
};

}  // namespace

TEST_CASE("uniform branches: nested, shared and stateful sides, flipping and per-lane conditions: interpreter == JIT == both sides computed") {
  uint32_t runs = 0, branches = 0, nested = 0;
  for (const char* src : kUnits) {
    const Compiled c = compileText(src);
    INFO(c.name);
    CHECK(!c.sched.branches.empty());
    branches += static_cast<uint32_t>(c.sched.branches.size());
    for (const Branch& b : c.sched.branches) nested += b.parent != kNone;
    for (const uint32_t every : {1u, 3u})  // flipping every block, and every third
      for (const uint32_t block : {64u, 37u, 1u}) {
        INFO("block " << block << ", params step every " << every);
        runs += fourWays(c, 3000, block, every).runs;
      }
    if (voicesOf(c) != 0)
      for (jit::VoiceCode vc : {jit::VoiceCode::Groups, jit::VoiceCode::Pairs256, jit::VoiceCode::Pairs512})
        runs += fourWays(c, 3000, 64, 2, vc).runs;
  }
  MESSAGE("uniform branches: " << runs << " runs bit-identical (" << branches << " branches, " << nested << " nested)");
  CHECK(nested >= 2);
}

TEST_CASE("uniform branches: every fixture and stock patch: interpreter == JIT == both sides computed, params stepping" *
          doctest::test_suite("slow")) {
  std::vector<std::string> skipped;
  uint32_t patches = 0, withBranches = 0, guarded = 0, runs = 0;
  for (const fs::path& path : corpusFiles())
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      INFO(c.file << ":" << c.name);
      ++patches;
      if (c.sched.branches.empty()) continue;
      ++withBranches;
      for (uint32_t g : c.sched.guardOf) guarded += g != kNone;
      const Four f = fourWays(c, 4000, 64, 2);
      if (f.resources) continue;
      runs += f.runs;
      runs += fourWays(c, 2000, 37, 1).runs;
    }
  MESSAGE("uniform branches: " << patches << " corpus patches, " << withBranches << " with branches (" << guarded
                               << " guarded ops), " << runs << " runs bit-identical");
  CHECK(withBranches >= 10);
  CHECK(runs >= 40);
}
