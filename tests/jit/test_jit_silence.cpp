// T1.x-silence: the interpreter and the JIT sleep identically. The decisions are the shared image host's (from buffer
// and state values that are bit-identical in both backends); the generated code only reads the segment flags and
// zero-fills a skipped region's outputs. Over the corpus with silences in the input: the same samples bit for bit
// and the same sleep statistics. (Instant interpreter → JIT swaps rely on it, Q108.)
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../engine/silence_harness.h"

using namespace silence_test;
namespace fs = std::filesystem;

TEST_CASE("jit silence: interpreter and JIT sleep and wake identically, bit for bit (T1.x-silence)") {
  REQUIRE(interp_test::rigImageBuilder());  // test_jit.cpp sets it to the JIT
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(interp_test::patchesDir())) files.push_back(p);
  const fs::path stdlib = fs::path(PQ_TEST_DATA_DIR).parent_path() / "stdlib";
  for (const auto& e : fs::recursive_directory_iterator(stdlib))
    if (e.path().extension() == ".pqp") files.push_back(e.path());
  std::vector<std::string> skipped;
  uint32_t compared = 0, slept = 0;
  uint64_t asleepBlocks = 0;
  for (const fs::path& path : files)
    for (const interp_test::Compiled& c : interp_test::compileFile(path, 48000.0, skipped)) {
      if (c.sched.sleep.segments.empty()) continue;
      INFO(c.file << ":" << c.name);
      const Input in = burstInput(20000 + 17);
      for (const std::vector<uint32_t>& blocks : {std::vector<uint32_t>{64}, std::vector<uint32_t>{37, 1, 128}}) {
        const Run interp = run(c, eng::SleepMode::On, in, 40000, blocks);
        const Run jit = run(c, eng::SleepMode::On, in, 40000, blocks, interp_test::rigImageBuilder());
        REQUIRE(interp.error.empty());
        REQUIRE(jit.error.empty());
        std::string where;
        CHECK_MESSAGE(sameBits(interp, jit, where), where);
        CHECK(interp.stats.sleeps == jit.stats.sleeps);
        CHECK(interp.stats.wakes == jit.stats.wakes);
        CHECK(interp.stats.asleepBlocks == jit.stats.asleepBlocks);
        CHECK(interp.stats.flushes == jit.stats.flushes);
        CHECK(interp.tailsDone == jit.tailsDone);
        CHECK(interp.dormant == jit.dormant);
        ++compared;
        slept += interp.stats.sleeps > 0 ? 1u : 0u;
        asleepBlocks += interp.stats.asleepBlocks;
      }
    }
  MESSAGE("interpreter vs JIT with sleep: " << compared << " runs, " << slept << " slept (" << asleepBlocks
                                            << " segment-blocks asleep), bit-identical");
  CHECK(compared >= 80);
  CHECK(slept >= 60);
}
