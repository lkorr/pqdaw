#include <cstdio>
#include <string_view>
#include <vector>

#include "pq/app/cli.h"
#include "pq/app/commands.h"
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
  switch (opts->mode) {
    case pq::app::Mode::Help: std::fputs(pq::app::usage().c_str(), stdout); return 0;
    case pq::app::Mode::Screenshot: return pq::app::runScreenshot(*opts);
    case pq::app::Mode::WindowSelfTest: return pq::app::runWindow(*opts, true);
    case pq::app::Mode::Window: return pq::app::runWindow(*opts, false);
    case pq::app::Mode::ListDevices: return pq::app::runListDevices(*opts);
    case pq::app::Mode::Sine: return pq::app::runSine(*opts);
    case pq::app::Mode::LatencyTest: return pq::app::runLatencyTest(*opts);
    case pq::app::Mode::Play: return pq::app::runPlay(*opts);
    case pq::app::Mode::AsioPanel: return pq::app::runAsioPanel(*opts);
    case pq::app::Mode::Render: return pq::app::runRender(*opts);
  }
  return 1;
}
