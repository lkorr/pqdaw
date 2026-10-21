#pragma once

// pqdaw command line. One "mode" flag picks what the process does; other flags modify it.
// To add a mode: add a Mode value, parse its flag in parseCli (cli.cpp), document it in usage(), and
// dispatch it in main.cpp.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "pq/core/result.h"

namespace pq::app {

enum class Mode {
  Window,          // default: open the main window (currently the test pattern)
  Screenshot,      // --screenshot <scene> <out.png> [--size WxH]
  WindowSelfTest,  // --window-selftest: scripted resize/minimise/restore run, exit code = result
  Help,            // --help
};

struct CliOptions {
  Mode mode = Mode::Window;
  std::string scene = "test-pattern";
  std::string outPath;
  uint32_t width = 0;   // 0 = mode default
  uint32_t height = 0;
  bool animate = false;  // --animate: continuous redraw (exercises frame pacing)
};

// Exit code a test runner treats as "skipped" (CTest SKIP_RETURN_CODE), e.g. no display for the window test.
inline constexpr int kExitSkipped = 77;

// `args` excludes argv[0].
Result<CliOptions> parseCli(std::span<const std::string_view> args);
// Parses "WxH" (both 1..16384).
bool parseSize(std::string_view s, uint32_t& w, uint32_t& h) noexcept;
std::string usage();

}  // namespace pq::app
