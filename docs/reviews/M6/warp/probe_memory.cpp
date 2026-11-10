// M6 review (warp) Q1: what the stretch pool / clip player / timeline actually commit (process private bytes), against
// the workers' estimates (StretchPool::memoryBytes(), "≈ 24 MiB per clip player", "+13.7 MiB for 32 mono").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "probe_common.h"
#include "pq/engine/stretch_pool.h"

static double privMiB() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<double>(pmc.PrivateUsage) / 1048576.0;
}
static double wsMiB() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<double>(pmc.WorkingSetSize) / 1048576.0;
}

template <class F>
static void measure(const char* name, F make) {
  const double p0 = privMiB(), w0 = wsMiB();
  const auto t0 = std::chrono::steady_clock::now();
  auto obj = make();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double p1 = privMiB(), w1 = wsMiB();
  std::printf("%-52s private +%7.2f MiB  working set +%7.2f MiB  (%.1f ms)\n", name, p1 - p0, w1 - w0, ms);
  obj.reset();
}

int main() {
  // Warm the CRT heap / sinc tables so they do not land on the first measurement.
  { auto warm = std::make_unique<e::ClipPlayer>(e::ClipPlayerConfig{.stretchVoices = 1, .stretchMonoVoices = 1}); }
  for (double sr : {48000.0, 96000.0}) {
    std::printf("---- %.0f Hz ----\n", sr);
    {
      e::StretchPool est(32, sr, 32);
      std::printf("StretchPool(32 stereo, 32 mono).memoryBytes() estimate: %.2f MiB\n",
                  static_cast<double>(est.memoryBytes()) / 1048576.0);
      e::StretchPool est2(32, sr, 0);
      std::printf("StretchPool(32 stereo).memoryBytes() estimate: %.2f MiB\n", static_cast<double>(est2.memoryBytes()) / 1048576.0);
    }
    measure("StretchPool(1 stereo)", [&] { return std::make_unique<e::StretchPool>(1, sr, 0); });
    measure("StretchPool(0 stereo, 1 mono)", [&] { return std::make_unique<e::StretchPool>(0, sr, 1); });
    measure("StretchPool(32 stereo)", [&] { return std::make_unique<e::StretchPool>(32, sr, 0); });
    measure("StretchPool(0 stereo, 32 mono)", [&] { return std::make_unique<e::StretchPool>(0, sr, 32); });
    measure("StretchPool(32 stereo, 32 mono)", [&] { return std::make_unique<e::StretchPool>(32, sr, 32); });
    measure("ClipPlayer default config (32+32 stretchers)", [&] {
      e::ClipPlayerConfig c;
      c.sampleRate = sr;
      c.slots = 64;
      return std::make_unique<e::ClipPlayer>(c);
    });
    measure("ClipPlayer default config, stretchers 0", [&] {
      e::ClipPlayerConfig c;
      c.sampleRate = sr;
      c.slots = 64;
      c.stretchVoices = c.stretchMonoVoices = 0;
      return std::make_unique<e::ClipPlayer>(c);
    });
    measure("Timeline default config (maxTracks 64)", [&] {
      e::TimelineConfig c;
      c.sampleRate = sr;
      return std::make_unique<e::Timeline>(c);
    });
  }
  return 0;
}
