#include <cstdio>
#include <string_view>
#include <vector>

#include "pq/app/cli.h"
#include "pq/app/commands.h"
#include "pq/app/jit_cache.h"
#include "pq/app/startup.h"

int main(int argc, char** argv) {
  pq::app::markProcessStart();  // first-frame timing (T3.7)
  pq::app::markStartupPhase("main() entered (exe load + CRT start)");
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  auto opts = pq::app::parseCli(args);
  pq::app::markStartupPhase("command line parsed");
  if (!opts) {
    std::fprintf(stderr, "pqdaw: %s\n\n%s", opts.error().message.c_str(), pq::app::usage().c_str());
    return 2;
  }
  // T1b.4: the modes that JIT-compile use the user's compile cache and the AOT seed next to the executable.
  using pq::app::Mode;
  if (opts->mode == Mode::Window || opts->mode == Mode::WindowSelfTest || opts->mode == Mode::Play || opts->mode == Mode::Render)
    pq::app::configureJitCache();
  switch (opts->mode) {
    case Mode::Help: std::fputs(pq::app::usage().c_str(), stdout); return 0;
    case Mode::Screenshot: return pq::app::runScreenshot(*opts);
    case Mode::WindowSelfTest: return pq::app::runWindow(*opts, true);
    case Mode::Window: return pq::app::runWindow(*opts, false);
    case Mode::ListDevices: return pq::app::runListDevices(*opts);
    case Mode::Sine: return pq::app::runSine(*opts);
    case Mode::LatencyTest: return pq::app::runLatencyTest(*opts);
    case Mode::Play: return pq::app::runPlay(*opts);
    case Mode::AsioPanel: return pq::app::runAsioPanel(*opts);
    case Mode::Render: return pq::app::runRender(*opts);
    case Mode::JitAot: return pq::app::runJitAot(opts->outPath);
  }
  return 1;
}
