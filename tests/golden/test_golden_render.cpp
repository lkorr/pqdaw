// Golden offline renders (T1.10): every patch in tests/golden/patches rendered by pqc's renderer with the
// interpreter AND the JIT backend must match the committed 32-bit float WAV in tests/golden/audio (absolute +
// spectral tolerance, golden_audio.h). Also: determinism, block-size invariance, and mutations that must fail.
//
// Regenerating the goldens (only after a deliberate change, reviewed by listening): set PQ_UPDATE_GOLDENS=1 and run
// pq_tests_render; the interpreter renders are written, then both backends are compared against them as usual.
// The same renders from the command line: tools/README.md lists the pqc command of each golden.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "golden_audio.h"
#include "pq/pqc/pqc.h"

namespace {

namespace fs = std::filesystem;
using pq::pqc::Backend;

struct GoldenCase {
  const char* name;      // tests/golden/audio/<name>.wav
  const char* file;      // tests/golden/patches/<file>
  const char* notes;     // "" for global patches
  double seconds;
};

// Keep in sync with tools/README.md ("Golden renders").
const GoldenCase kCases[] = {
    {"fm_voice", "fm_voice.pqp", "60@0:0.4 64@0.1:0.4 67@0.2:0.5*0.9 72@0.45:0.35*0.6 55@0.6:0.3", 1.0},
    {"fm_voice_arp", "fm_voice.pqp", "60 64 67 72", 1.0},
    {"pluck", "pluck.pqp", "48@0:0.5 55@0.15:0.5*0.8 64@0.3:0.4*0.9 67@0.5:0.4", 1.0},
    {"drive_zdf", "drive_zdf.pqp", "", 0.5},
    {"stereo_echo", "stereo_echo.pqp", "", 1.0},
    {"noise_tables", "noise_tables.pqp", "", 0.5},
};

fs::path patchPath(const GoldenCase& c) { return golden::goldenDir() / "patches" / c.file; }
fs::path wavPath(const GoldenCase& c) { return golden::goldenDir() / "audio" / (std::string(c.name) + ".wav"); }

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && std::strcmp(v, "1") == 0;
}

pq::pqc::RenderOptions optionsFor(const GoldenCase& c, Backend backend, uint32_t block = 64) {
  pq::pqc::RenderOptions o;
  o.backend = backend;
  o.seconds = c.seconds;
  o.blockFrames = block;
  o.notes = c.notes;
  return o;
}

pq::engine::BufferSink render(const fs::path& patch, const pq::pqc::RenderOptions& o) {
  pq::engine::BufferSink sink;
  auto r = pq::pqc::renderFile(patch, o, sink);
  if (!r) FAIL(r.error().message);
  CHECK(r->stats.rejectedBlocks == 0);
  return sink;
}

const char* backendName(Backend b) { return b == Backend::Jit ? "jit" : "interp"; }

}  // namespace

TEST_CASE("golden renders: interpreter and JIT match the committed WAVs") {
  const bool update = updateGoldens();
  int checked = 0;
  for (const GoldenCase& c : kCases) {
    if (update) {
      const auto sink = render(patchPath(c), optionsFor(c, Backend::Interpreter));
      REQUIRE(pq::engine::writeWav(wavPath(c), sink.channels, sink.sampleRate));
      MESSAGE("updated " << wavPath(c).string());
    }
    auto gold = pq::engine::readWav(wavPath(c));
    if (!gold) FAIL(gold.error().message << " (generate with PQ_UPDATE_GOLDENS=1)");
    CHECK(fs::file_size(wavPath(c)) < 1024 * 1024);  // goldens stay small (TASKS T1.10)
    CHECK(gold->isFloat);
    CHECK(gold->bitsPerSample == 32);
    for (Backend b : {Backend::Interpreter, Backend::Jit}) {
      const auto sink = render(patchPath(c), optionsFor(c, b));
      const golden::Comparison cmp = golden::compare(*gold, sink.channels, sink.sampleRate);
      INFO((std::string(c.name) + " [" + backendName(b) + "]: " + cmp.message));
      CHECK(cmp.ok);
      MESSAGE((std::string(c.name) + " [" + backendName(b) + "]: " + cmp.message));
      // Not silent, not clipping wildly: a golden of silence would prove nothing.
      double peak = 0.0;
      for (const auto& ch : sink.channels)
        for (float v : ch) peak = std::max(peak, static_cast<double>(std::abs(v)));
      CHECK(peak > 0.05);
      CHECK(peak < 4.0);
      ++checked;
    }
  }
  CHECK(checked == 2 * static_cast<int>(std::size(kCases)));
}

TEST_CASE("golden renders: the same request renders bit-identical output") {
  // Determinism (SPEC §9.8): fixed block size and seeded noise, no wall clock, a fresh image per render (chunk-size
  // independence is tested on renderOffline itself, tests/engine/test_offline_render.cpp).
  for (const GoldenCase& c : kCases) {
    for (Backend b : {Backend::Interpreter, Backend::Jit}) {
      const auto a = render(patchPath(c), optionsFor(c, b));
      const auto again = render(patchPath(c), optionsFor(c, b));
      REQUIRE(a.channels.size() == again.channels.size());
      for (size_t ch = 0; ch < a.channels.size(); ++ch) {
        REQUIRE(a.channels[ch].size() == again.channels[ch].size());
        CHECK(std::memcmp(a.channels[ch].data(), again.channels[ch].data(), a.channels[ch].size() * 4) == 0);
      }
    }
  }
}

TEST_CASE("golden renders: block size does not change the result beyond the tolerance") {
  // Events are sample accurate and params constant here, so only float rounding in block-dependent paths
  // (voice compaction, sub-block loops) may differ.
  for (const GoldenCase& c : kCases) {
    auto gold = pq::engine::readWav(wavPath(c));
    REQUIRE(gold);
    for (uint32_t block : {16u, 128u, 1024u}) {
      const auto sink = render(patchPath(c), optionsFor(c, Backend::Interpreter, block));
      const golden::Comparison cmp = golden::compare(*gold, sink.channels, sink.sampleRate);
      INFO((std::string(c.name) + " block " + std::to_string(block) + ": " + cmp.message));
      CHECK(cmp.ok);
    }
  }
}

namespace {

// The golden patch with `from` replaced by `to` (exactly once), written to a temp file.
fs::path mutatedPatch(const GoldenCase& c, const std::string& from, const std::string& to) {
  std::ifstream in(patchPath(c), std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  const size_t k = text.find(from);
  REQUIRE(k != std::string::npos);
  REQUIRE(text.find(from, k + 1) == std::string::npos);
  text.replace(k, from.size(), to);
  const fs::path out = fs::temp_directory_path() / ("pq_golden_mutant_" + std::string(c.name) + ".pqp");
  std::ofstream(out, std::ios::binary) << text;
  return out;
}

const GoldenCase& caseNamed(const char* name) {
  for (const GoldenCase& c : kCases)
    if (std::strcmp(c.name, name) == 0) return c;
  FAIL("no case " << name);
  return kCases[0];
}

}  // namespace

TEST_CASE("golden renders: changed patches, notes and engine behaviour fail the comparison" * doctest::test_suite("slow")) {
  struct Mutation {
    const char* golden;
    const char* from;
    const char* to;
  };
  // Small but real changes: a modulator ratio 2 → 2.05, the envelope release, a filter cutoff, an echo time by
  // 2 ms, the noise filter coefficient, the pluck's damping.
  const Mutation mutations[] = {
      {"fm_voice", "param ratio = 2.0", "param ratio = 2.05"},
      {"fm_voice", "param release = 120ms", "param release = 100ms"},
      {"pluck", "param damp  = 0.5", "param damp  = 0.45"},
      {"drive_zdf", "param cutoff = 1200Hz", "param cutoff = 1250Hz"},
      {"stereo_echo", "delay(left * fb, 270ms, 500ms)", "delay(left * fb, 272ms, 500ms)"},
      {"noise_tables", "param tone = 0.08", "param tone = 0.085"},
  };
  for (const Mutation& m : mutations) {
    const GoldenCase& c = caseNamed(m.golden);
    auto gold = pq::engine::readWav(wavPath(c));
    REQUIRE(gold);
    const fs::path mutant = mutatedPatch(c, m.from, m.to);
    const auto sink = render(mutant, optionsFor(c, Backend::Interpreter));
    const golden::Comparison cmp = golden::compare(*gold, sink.channels, sink.sampleRate);
    INFO((std::string(m.golden) + ": " + m.from + " -> " + m.to + ": " + cmp.message));
    CHECK_FALSE(cmp.ok);
    // The spectral check alone must catch it too (what remains when a test relaxes absTol).
    golden::Tolerance specOnly;
    specOnly.absTol = 1e30;
    CHECK_FALSE(golden::compare(*gold, sink.channels, sink.sampleRate, specOnly).ok);
    fs::remove(mutant);
  }
  // Different notes (one note a semitone higher) and a velocity change.
  {
    const GoldenCase& c = caseNamed("fm_voice");
    auto gold = pq::engine::readWav(wavPath(c));
    REQUIRE(gold);
    pq::pqc::RenderOptions o = optionsFor(c, Backend::Interpreter);
    o.notes = "60@0:0.4 65@0.1:0.4 67@0.2:0.5*0.9 72@0.45:0.35*0.6 55@0.6:0.3";
    CHECK_FALSE(golden::compare(*gold, render(patchPath(c), o).channels, 48000).ok);
    o.notes = "60@0:0.4 64@0.1:0.4 67@0.2:0.5*0.8 72@0.45:0.35*0.6 55@0.6:0.3";
    CHECK_FALSE(golden::compare(*gold, render(patchPath(c), o).channels, 48000).ok);
  }
  // Engine-side regressions simulated on the render: one sample late, −0.3 dB, a dropped block.
  {
    const GoldenCase& c = caseNamed("fm_voice");
    auto gold = pq::engine::readWav(wavPath(c));
    REQUIRE(gold);
    const auto good = render(patchPath(c), optionsFor(c, Backend::Interpreter));
    REQUIRE(golden::compare(*gold, good.channels, 48000).ok);
    auto late = good.channels;
    late[0].insert(late[0].begin(), 0.0f);
    late[0].pop_back();
    CHECK_FALSE(golden::compare(*gold, late, 48000).ok);
    auto quieter = good.channels;
    for (float& v : quieter[0]) v *= 0.966f;  // −0.3 dB
    CHECK_FALSE(golden::compare(*gold, quieter, 48000).ok);
    golden::Tolerance specOnly;
    specOnly.absTol = 1e30;
    CHECK_FALSE(golden::compare(*gold, quieter, 48000, specOnly).ok);
    auto dropped = good.channels;
    std::fill(dropped[0].begin() + 12000, dropped[0].begin() + 12064, 0.0f);
    CHECK_FALSE(golden::compare(*gold, dropped, 48000).ok);
    // …while a harmless 1e-6 perturbation passes.
    auto nudged = good.channels;
    for (size_t i = 0; i < nudged[0].size(); ++i) nudged[0][i] += (i & 1 ? 1e-6f : -1e-6f);
    CHECK(golden::compare(*gold, nudged, 48000).ok);
  }
}
