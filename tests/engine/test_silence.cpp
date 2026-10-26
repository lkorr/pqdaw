// T1.x-silence (SPEC §7.7): sleeping regions at run time (pq/engine/image_host.h "sleep"), on the image builder of
// the executable (the interpreter in pq_tests_engine; tests/jit/test_jit_silence.cpp compares it with the JIT).
//   - a feed-forward chain (delay line, z1s, a smoothed gain) sleeps once its tail has passed and wakes sample-
//     accurately when its input resumes mid-block: identical to a run that never sleeps;
//   - a feedback chain (combs, a Schroeder allpass, a one-pole) sleeps once its tail is below −120 dBFS: identical to
//     the Shadow run (same decisions, every region still running), within the cut residue of a never-sleeping run;
//   - a param change wakes a sleeping smoother; RtScope clean; the silence reports;
//   - every patch of the corpus (tests/graph/cases, tests/engine/patches, stdlib): Shadow runs never see a region it
//     would have skipped produce anything but zeros, and sleeping runs equal the Shadow runs.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "silence_harness.h"

using namespace silence_test;
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kResume = 64 * 1200 + 17;  // mid-block for 64-frame blocks (and for the irregular pattern)

Input burst() {
  return [](const eng::IoSlot&, bool, bool, uint64_t t) -> float {
    const bool sounding = t < 4800 || (t >= kResume && t < kResume + 3000);
    return sounding ? static_cast<float>(0.7 * std::sin(0.0371 * static_cast<double>(t))) : 0.0f;
  };
}

const char* kFeedForward = R"(
patch FeedForward {
  in    x : audio
  param vol = 0.8 [0..1]
  out   y : audio
  e = delay(x, 30ms, 40ms) * 0.5
  f = x * 0.25 + z1(x) * 0.125 + z1(z1(x)) * 0.0625
  y = gain(e + f, vol, 2ms)
}
)";

const char* kFeedback = R"(
patch Verb {
  in    x : audio
  param fb = 0.7 [0..0.95]
  out   y : audio
  c1 = delay(x + c1 * fb, 7.1ms, 20ms)
  c2 = delay(x + c2 * fb, 9.7ms, 20ms)
  m  = (c1 + c2) * 0.5
  d  = delay(w, 2.3ms, 5ms)
  w  = m + d * 0.5
  a  = d - w * 0.5
  s  = z1(s * 0.9 + a * 0.1)
  y  = x + s
}
)";

uint64_t firstSleepFrame(const Run& r) {
  for (size_t b = 0; b < r.tailsDone.size(); ++b)
    if (r.tailsDone[b]) return r.blockStart[b];
  return ~uint64_t{0};
}

}  // namespace

TEST_CASE("silence: a feed-forward chain sleeps after its tail and wakes sample-accurately (T1.x-silence)") {
  const interp_test::Compiled c = interp_test::compileText(kFeedForward);
  REQUIRE(c.sched.sleep.segments.size() == 1);
  for (const std::vector<uint32_t>& blocks : {std::vector<uint32_t>{64}, std::vector<uint32_t>{64, 37, 1, 100, 13}}) {
    INFO("blocks " << blocks.size());
    const Run on = run(c, eng::SleepMode::On, burst(), 96000, blocks, interp_test::rigImageBuilder());
    const Run off = run(c, eng::SleepMode::Off, burst(), 96000, blocks, interp_test::rigImageBuilder());
    REQUIRE(on.error.empty());
    REQUIRE(off.error.empty());
    CHECK(on.stats.sleeps >= 2);           // after the first burst and after the second
    CHECK(on.stats.wakes >= 1);            // at the resume, mid-block
    CHECK(on.stats.asleepBlocks > 500);
    CHECK(off.stats.sleeps == 0);
    std::string where;
    CHECK_MESSAGE(sameValues(on, off, where), where);  // identical to never sleeping: a feed-forward tail ends exactly
    // The output after the resume is the sound itself (not a block late).
    CHECK(on.out[0][kResume] != 0.0f);
    CHECK(on.out[0][kResume] == off.out[0][kResume]);
    CHECK(on.rtViolations == 0);
    // It sleeps once its tail (the delay line's reach: its 40 ms maximum + 3 frames) has passed after the burst's
    // end, at the next block boundary, not before.
    const uint64_t first = firstSleepFrame(on);
    CHECK(first >= 4800 + 1923);
    CHECK(first <= 4800 + 1923 + 2 * 100);
    // Dormant once asleep with a silent output; never while the tail rings.
    for (size_t b = 0; b < on.dormant.size(); ++b)
      if (on.dormant[b]) CHECK(on.tailsDone[b]);
  }
}

TEST_CASE("silence: a feedback chain sleeps once its tail is below -120 dBFS (T1.x-silence)") {
  const interp_test::Compiled c = interp_test::compileText(kFeedback);
  REQUIRE(c.sched.sleep.segments.size() == 1);
  CHECK(c.sched.sleep.segments[0].feedback);
  const std::vector<uint32_t> blocks{64, 37, 1, 100, 13};
  const Run on = run(c, eng::SleepMode::On, burst(), 130000, blocks, interp_test::rigImageBuilder());
  const Run shadow = run(c, eng::SleepMode::Shadow, burst(), 130000, blocks, interp_test::rigImageBuilder());
  const Run off = run(c, eng::SleepMode::Off, burst(), 130000, blocks, interp_test::rigImageBuilder());
  REQUIRE(on.error.empty());
  REQUIRE(shadow.error.empty());
  CHECK(on.stats.sleeps >= 2);
  CHECK(on.stats.wakes >= 1);
  CHECK(shadow.stats.sleeps == on.stats.sleeps);
  CHECK(shadow.stats.violations == 0);
  std::string where;
  // Skipping is exact: the same decisions and flushes, every region running, give the same samples.
  CHECK_MESSAGE(sameValues(on, shadow, where), where);
  // Against never sleeping: equal until the first cut, then within the cut residue (≤ −120 dBFS).
  const uint64_t first = firstSleepFrame(on);
  REQUIRE(first < kResume);
  for (uint64_t t = 0; t < first; ++t) REQUIRE(on.out[0][t] == off.out[0][t]);
  const double d = maxAbsDiff(on, off);
  MESSAGE("feedback chain: first sleep at frame " << first << ", max |sleeping - never sleeping| " << d);
  CHECK(d <= 4e-6);
  CHECK(on.rtViolations == 0);
}

TEST_CASE("silence: a param change wakes a sleeping smoother; it sleeps again once settled (T1.x-silence)") {
  const interp_test::Compiled c = interp_test::compileText(kFeedForward);
  // vol moves twice while the input is silent and once just before the input resumes.
  auto params = [](uint64_t t, std::vector<float>& p) {
    p[0] = t < 20000 ? 0.8f : t < 40000 ? 0.3f : t < kResume - 100 ? 1.0f : 0.5f;
  };
  const std::vector<uint32_t> blocks{64};
  const Run on = run(c, eng::SleepMode::On, burst(), 96000, blocks, interp_test::rigImageBuilder(), params);
  const Run off = run(c, eng::SleepMode::Off, burst(), 96000, blocks, interp_test::rigImageBuilder(), params);
  REQUIRE(on.error.empty());
  std::string where;
  CHECK_MESSAGE(sameValues(on, off, where), where);
  // Woken by the changes at 20000 and 40000; the one 100 frames before the input resumes wakes it too (the input
  // then finds it awake, its gain still gliding); asleep again after each glide and after the second burst.
  CHECK(on.stats.wakes == 3);
  CHECK(on.stats.sleeps == 4);
}

TEST_CASE("silence: corpus — skipped regions would have produced zeros, sleeping equals Shadow (T1.x-silence)") {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(interp_test::patchesDir())) files.push_back(p);
  const fs::path stdlib = fs::path(PQ_TEST_DATA_DIR).parent_path() / "stdlib";
  for (const auto& e : fs::recursive_directory_iterator(stdlib))
    if (e.path().extension() == ".pqp") files.push_back(e.path());
  std::vector<std::string> skipped;
  uint32_t planned = 0, slept = 0, patches = 0;
  for (const fs::path& path : files)
    for (const interp_test::Compiled& c : interp_test::compileFile(path, 48000.0, skipped)) {
      ++patches;
      if (c.sched.sleep.segments.empty()) continue;
      ++planned;
      INFO(c.file << ":" << c.name);
      const Input in = burstInput(20000 + 17);
      const std::vector<uint32_t> blocks{64, 37, 128};
      const Run shadow = run(c, eng::SleepMode::Shadow, in, 40000, blocks, interp_test::rigImageBuilder());
      const Run on = run(c, eng::SleepMode::On, in, 40000, blocks, interp_test::rigImageBuilder());
      REQUIRE(shadow.error.empty());
      REQUIRE(on.error.empty());
      CHECK(shadow.stats.violations == 0);
      std::string where;
      CHECK_MESSAGE(sameValues(on, shadow, where), where);
      CHECK(on.rtViolations == 0);
      slept += on.stats.sleeps > 0 ? 1u : 0u;
    }
  MESSAGE("corpus: " << patches << " patches, " << planned << " with sleep segments, " << slept << " slept");
  CHECK(planned >= 40);
  CHECK(slept >= 30);
}
