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
  Screenshot,      // --screenshot <scene> <out.png> [--size WxH] [--scale S]
  WindowSelfTest,  // --window-selftest: scripted resize/minimise/restore run, exit code = result
  Help,            // --help
  ListDevices,     // --list-devices [--backend wasapi|rtaudio] [--asio]
  Sine,            // --sine <hz> [--seconds <s>] [--device <id|endpoint>] [--backend wasapi|rtaudio] [--asio] [--rate <hz>]
                   //   [--buffer <n>] [--block <n>] [--exclusive] [--level <dBFS>] [--inject-stall <wake>]
  LatencyTest,     // --latency-test [--seconds <s>] [--device <id|endpoint>] [--buffer <n>] [--level <dBFS>]
  Play,            // --play <file.pqp> [--patch <name>] [--seconds <s>] [--level <dBFS>] + the --sine stream flags
};

// Which WASAPI implementation --sine / --list-devices use (--asio overrides both with RtAudio ASIO).
enum class WasapiImpl : uint8_t {
  Own,      // --backend wasapi (default): pq::engine::WasapiDevice
  RtAudio,  // --backend rtaudio: RtAudio's WASAPI backend
};

struct CliOptions {
  Mode mode = Mode::Window;
  std::string scene = "test-pattern";
  std::string outPath;
  uint32_t width = 0;   // 0 = mode default
  uint32_t height = 0;
  bool animate = false;  // --animate: continuous redraw (exercises frame pacing)
  float scale = 0.0f;    // --scale: UI scale (0 = default: 1.0 for --screenshot, the display scale for the window)
  // T3.7: --project <dir>: the project the DAW shell opens (window: the scene defaults to "shell"; --screenshot of a
  // view scene renders it). --idle-check <s>: the shell window measures frames and process CPU over s seconds of
  // idling (after a 1 s settle), prints them and exits 0 only for 0 frames at < 0.1 % CPU.
  std::string project;
  double idleCheckSeconds = 0.0;

  // Audio (--list-devices, --sine).
  double sineHz = 440.0;
  double seconds = 3.0;
  uint32_t device = 0xFFFFFFFFu;  // pq::engine::kDefaultDevice
  std::string deviceEndpoint;     // --device {endpoint id} (WASAPI); overrides device when non-empty
  WasapiImpl wasapi = WasapiImpl::Own;
  bool asio = false;
  double levelDb = -24.0;         // test signal level, dBFS
  uint64_t stallAfterWake = 0;    // --inject-stall: WasapiDevice test hook (0 = off)
  bool exclusive = false;
  uint32_t sampleRate = 0;      // 0 = the device's preferred rate
  uint32_t bufferFrames = 256;  // device buffer request
  uint32_t blockFrames = 32;    // internal engine block (pq::engine::BlockAdapter)

  // --play (T1.7): the patch file and the patch in it (empty: the first one).
  std::string playPath;
  std::string patchName;
};

// Exit code a test runner treats as "skipped" (CTest SKIP_RETURN_CODE), e.g. no display for the window test.
inline constexpr int kExitSkipped = 77;

// `args` excludes argv[0].
Result<CliOptions> parseCli(std::span<const std::string_view> args);
// Parses "WxH" (both 1..16384).
bool parseSize(std::string_view s, uint32_t& w, uint32_t& h) noexcept;
std::string usage();

}  // namespace pq::app
