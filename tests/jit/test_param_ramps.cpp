// TX.smooth: param ramps (pq/graph/schedule.h "Param ramps") on both backends. Params step every few blocks (knob
// drags, automation, macros as the engine delivers them) over the T1.7 corpus and every stdlib patch (filters, the
// stock devices): the interpreter and the JIT compute the same samples bit for bit, with no allocation on the audio
// thread; a step at an audio-rate input becomes the documented 20 ms linear ramp.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/dsp/atoms.h"

using namespace interp_test;
namespace fs = std::filesystem;

namespace {

uint32_t voicesOf(const Compiled& c) {
  for (const Stage& s : c.sched.stages)
    if (s.poly == Poly::Voice) return 11;  // two groups, the second one partly active
  return 0;
}

// Deterministic param motion: every `every` blocks a param jumps to a new value inside its range (bools toggle);
// some blocks move several params at once, some none, so ramps start, restart mid-way and settle.
float steppedParam(uint32_t slot, const ParamSlot& p, uint64_t t0, float base, uint32_t every) {
  const uint64_t step = t0 / (64u * every);
  if (step == 0) return base;
  uint64_t h = (step * 0x9E3779B97F4A7C15ull) ^ (uint64_t{slot} * 0xBF58476D1CE4E5B9ull);
  h ^= h >> 31;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 29;
  if ((h & 3u) == 0) return base;  // a quarter of the steps leave it where it started
  if (p.kind == Kind::Bool) return (h >> 8) & 1u ? 1.0f : 0.0f;
  const double u = static_cast<double>((h >> 11) & 0xFFFFF) / static_cast<double>(0xFFFFF);
  if (p.hasRange) return static_cast<float>(p.rangeMin + u * (p.rangeMax - p.rangeMin));
  const double b = std::isfinite(base) ? base : p.defaultValue;
  return static_cast<float>(b * (0.5 + u));
}

uint64_t bitDiffs(const Streams& a, const Streams& b, std::string& where) {
  uint64_t n = 0;
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

std::vector<fs::path> rampCorpus() {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(patchesDir())) files.push_back(p);
  for (const fs::path& p : text_util::pqpFiles(fs::path(PQ_TEST_DATA_DIR) / "jit" / "patches")) files.push_back(p);
  for (const fs::path& p : text_util::pqpFiles(text_util::retiredDevicesDir())) files.push_back(p);
  const fs::path stdlib = fs::path(PQ_TEST_DATA_DIR).parent_path() / "stdlib";
  for (const auto& e : fs::recursive_directory_iterator(stdlib))
    if (e.path().extension() == ".pqp") files.push_back(e.path());
  return files;
}

void interpEqualsJit(const std::vector<uint32_t>& blocks, uint32_t every, uint32_t frames, uint32_t& compared,
                     uint32_t& withRamps) {
  REQUIRE(rigImageBuilder());  // test_jit.cpp sets it to the JIT
  std::vector<std::string> skipped;
  for (const fs::path& path : rampCorpus())
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      if (c.sched.params.empty()) continue;
      Setup st(c, voicesOf(c), 0);
      st.paramAt = [every](uint32_t slot, const ParamSlot& p, uint64_t t0, float base) {
        return steppedParam(slot, p, t0, base, every);
      };
      for (const uint32_t block : blocks) {
        INFO(c.file << ":" << c.name << " block " << block);
        const InterpRun interp = runInterp(st, frames, block, 1024);
        if (!interp.error.empty() && interp.error.find("resource") != std::string::npos) break;  // needs tables
        REQUIRE_MESSAGE(interp.error.empty(), interp.error);
        const InterpRun jit = runInterp(st, frames, block, 1024, rigImageBuilder());
        REQUIRE_MESSAGE(jit.error.empty(), jit.error);
        CHECK(interp.rtViolations == 0);
        CHECK(jit.rtViolations == 0);
        std::string where;
        CHECK_MESSAGE(bitDiffs(interp.streams, jit.streams, where) == 0, "not bit-identical at " << where);
        ++compared;
      }
      withRamps += c.sched.ramps.empty() ? 0u : 1u;
    }
}

}  // namespace

TEST_CASE("param ramps: interpreter == JIT bit for bit with params stepping every few blocks (corpus + stdlib)") {
  uint32_t compared = 0, withRamps = 0;
  interpEqualsJit({64, 37}, 3, 6000, compared, withRamps);  // ramps restart before they arrive
  interpEqualsJit({64}, 20, 6000, compared, withRamps);      // ramps arrive and the params rest
  MESSAGE("param ramps: " << compared << " runs, " << withRamps << " patches with ramps, bit-identical, no allocation");
  CHECK(compared >= 220);
  CHECK(withRamps >= 100);
}

TEST_CASE("param ramps: interpreter == JIT with every block size and fast motion" * doctest::test_suite("slow")) {
  uint32_t compared = 0, withRamps = 0;
  interpEqualsJit({1, 7, 64, 128, 1024}, 1, 4000, compared, withRamps);
  interpEqualsJit({1, 37, 1024}, 20, 8000, compared, withRamps);
  MESSAGE("param ramps (slow): " << compared << " runs, " << withRamps << " patches with ramps");
  CHECK(compared >= 500);
}

TEST_CASE("param ramps: a step at an audio-rate input is a 20 ms linear ramp with exact ends, on both backends") {
  const Compiled c = compileText(R"(patch S {
  in x : audio
  param g = 0 [0..1]
  out y : audio
  y = x * g
})");
  REQUIRE(c.sched.ramps.size() == 1);
  for (int backend = 0; backend < 2; ++backend) {
    INFO((backend ? "jit" : "interpreter"));
    Setup st(c, 0, 0);
    // x = 1 + small ripple; g: 0, then 1 from frame 640 (block 10), then 0.25 mid-ramp at 1280.
    st.paramAt = [](uint32_t, const ParamSlot&, uint64_t t0, float) { return t0 < 640 ? 0.0f : t0 < 1280 ? 1.0f : 0.25f; };
    const InterpRun r = runInterp(st, 4096, 64, 1024, backend ? rigImageBuilder() : ImageBuilder{});
    REQUIRE(r.error.empty());
    CHECK(r.rtViolations == 0);
    const std::vector<double>& y = r.streams.begin()->second;
    std::vector<double> x(y.size());
    for (size_t t = 0; t < x.size(); ++t) x[t] = static_cast<float>(st.root(0, 0, 0, t));
    auto g = [&](size_t t) { return y[t] / x[t]; };
    for (size_t t = 0; t < 640; ++t) CHECK(y[t] == 0.0);
    // 1 / 960 per frame from frame 640: at 640 + k the gain is (k + 1) / 960 (within float rounding).
    for (size_t k = 0; k < 640; ++k) CHECK(g(640 + k) == doctest::Approx((k + 1) / 960.0).epsilon(1e-5));
    // Retargeted at 1280 from where it was (640/960): to 0.25 over the next 960 frames, no jump.
    const double at = 640.0 / 960.0;
    CHECK(g(1280) == doctest::Approx(at + (0.25 - at) / 960.0).epsilon(1e-4));
    for (size_t t = 1281; t < 1280 + 960; ++t) CHECK(std::fabs(g(t) - g(t - 1)) < 1.0 / 900.0);
    for (size_t t = 1280 + 960; t < y.size(); ++t) CHECK(y[t] == static_cast<float>(x[t] * 0.25f));
  }
}

TEST_CASE("param ramps: a smoothed gain's target ramps like any input (x · g and gain(x, g) agree), on both backends") {
  const Compiled c = compileText(R"(patch G {
  in x : audio
  param g = 0 [0..1]
  out y : audio
  out z : audio
  out w : audio
  y = x * g
  z = gain(x, g, start: 1)
  w = gain_add(x, x, g, start: 1)
})");
  for (int backend = 0; backend < 2; ++backend) {
    INFO((backend ? "jit" : "interpreter"));
    Setup st(c, 0, 0);
    st.paramAt = [](uint32_t, const ParamSlot&, uint64_t t0, float) { return t0 < 640 ? 0.0f : t0 < 1280 ? 1.0f : 0.25f; };
    const InterpRun r = runInterp(st, 4096, 64, 1024, backend ? rigImageBuilder() : ImageBuilder{});
    REQUIRE(r.error.empty());
    std::vector<const std::vector<double>*> outs;
    for (const auto& [k, v] : r.streams) outs.push_back(&v);
    REQUIRE(outs.size() == 3);
    double maxDiff = 0.0, maxDiffAdd = 0.0;
    for (size_t t = 0; t < outs[0]->size(); ++t) {
      const double x = static_cast<float>(st.root(0, 0, 0, t));
      maxDiff = std::max(maxDiff, std::fabs((*outs[0])[t] - (*outs[1])[t]));
      maxDiffAdd = std::max(maxDiffAdd, std::fabs((*outs[2])[t] - x - (*outs[0])[t]));
    }
    CHECK(maxDiff < 1e-6);
    CHECK(maxDiffAdd < 1e-6);
  }
}

TEST_CASE("param ramps: a voice's param-rate values ramp per lane, and resting params read the pattern (both backends)") {
  const Compiled c = compileText(R"(patch V {
  param lvl = 0.5 [0..1]
  out y : audio voice
  y = sine(phasor(voice.hz)) * (lvl * voice.velocity)
})");
  REQUIRE(c.sched.ramps.size() == 1);
  Setup st(c, 11, 0);
  st.paramAt = [](uint32_t, const ParamSlot&, uint64_t t0, float) { return t0 < 1000 ? 0.5f : 1.0f; };
  const InterpRun interp = runInterp(st, 3000, 64, 1024);
  const InterpRun jit = runInterp(st, 3000, 64, 1024, rigImageBuilder());
  REQUIRE(interp.error.empty());
  REQUIRE(jit.error.empty());
  std::string where;
  CHECK_MESSAGE(bitDiffs(interp.streams, jit.streams, where) == 0, where);
  CHECK(interp.rtViolations == 0);
  CHECK(jit.rtViolations == 0);
}

// TX.smooth-fix1 (pq/graph/schedule.h "Param ramps"): zero at rest by construction. A stage runs its ramp mode only in
// the blocks a change reaches (the first block, a changed param or global param-rate source, for the ramp's 20 ms);
// otherwise the ramp-free code, whose output is the ramp-free schedule's bit for bit. A param held at its value
// (automation holding a breakpoint, a knob let go) never starts one.
namespace {
Compiled withoutRampsHere(const Compiled& c) {
  Compiled r;
  r.file = c.file;
  r.name = c.name;
  r.typed = c.typed;
  ScheduleOptions so;
  so.sampleRate = c.sched.sampleRate;
  so.paramRamps = false;
  auto s = schedule(r.typed, so);
  REQUIRE(s.ok());
  r.sched = std::move(s.value());
  return r;
}
}  // namespace

TEST_CASE("param ramps: ramp mode only while a change ramps; at rest the ramp-free program, bit for bit (both backends)") {
  constexpr uint32_t kRampBlocks = 960 / 64;  // 20 ms at 48 kHz in 64-frame blocks
  const Compiled g = compileText(R"(patch S {
  in x : audio
  param g = 0.5 [0..1]
  out y : audio
  out z : audio
  y = x * g
  z = x * (g * transport.bpm / 120)
})");
  const Compiled v = compileText(R"(patch V {
  param lvl = 0.5 [0..1]
  out y : audio voice
  y = sine(phasor(voice.hz)) * (lvl * voice.velocity)
})");
  REQUIRE(g.sched.ramps.size() == 2);
  for (const RampStage& rs : g.sched.rampStages) CHECK_FALSE(rs.checked);  // transport.bpm: followed by the host
  REQUIRE(v.sched.ramps.size() == 1);
  for (int backend = 0; backend < 2; ++backend) {
    INFO((backend ? "jit" : "interpreter"));
    const ImageBuilder build = backend ? rigImageBuilder() : ImageBuilder{};
    for (const Compiled* c : {&g, &v}) {
      INFO(c->name);
      const uint32_t runsPerBlock = c == &v ? 2u : 1u;  // the voice stage: two groups (11 voices)
      const uint32_t voices = c == &v ? 11u : 0u;
      {  // held: the same value every block
        Setup st(*c, voices, 0);
        st.paramAt = [](uint32_t, const ParamSlot&, uint64_t, float) { return 0.5f; };
        const InterpRun r = runInterp(st, 64 * 60, 64, 1024, build);
        REQUIRE(r.error.empty());
        CHECK(r.rtViolations == 0);
        REQUIRE(r.rampRunsPerBlock.size() == 60);
        for (uint32_t b = 0; b < 60; ++b) CHECK(r.rampRunsPerBlock[b] == (b < kRampBlocks ? runsPerBlock : 0u));
        Compiled plain = withoutRampsHere(*c);
        Setup sp(plain, voices, 0);
        sp.paramAt = st.paramAt;
        const InterpRun p = runInterp(sp, 64 * 60, 64, 1024, build);
        REQUIRE(p.error.empty());
        CHECK(p.rampRuns == 0);
        std::string where;
        CHECK_MESSAGE(bitDiffs(r.streams, p.streams, where) == 0, "not the ramp-free output at " << where);
      }
      {  // a step at block 20: ramp mode for the 20 ms that follow, then the ramp-free code again
        Setup st(*c, voices, 0);
        st.paramAt = [](uint32_t, const ParamSlot&, uint64_t t0, float) { return t0 < 64 * 20 ? 0.5f : 0.75f; };
        const InterpRun r = runInterp(st, 64 * 60, 64, 1024, build);
        REQUIRE(r.error.empty());
        CHECK(r.rtViolations == 0);
        REQUIRE(r.rampRunsPerBlock.size() == 60);
        for (uint32_t b = 0; b < 60; ++b) {
          const bool on = b < kRampBlocks || (b >= 20 && b < 20 + kRampBlocks);
          CHECK(r.rampRunsPerBlock[b] == (on ? runsPerBlock : 0u));
        }
      }
    }
  }
}
