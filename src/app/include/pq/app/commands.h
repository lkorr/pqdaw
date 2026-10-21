#pragma once

// What each CLI mode does. Return values are process exit codes (0 ok, 1 failure, kExitSkipped).

#include "pq/app/cli.h"

namespace pq::app {

int runScreenshot(const CliOptions& options);
// Interactive window (selfTest = false) or the scripted resize/minimise/restore check (selfTest = true).
int runWindow(const CliOptions& options, bool selfTest);

}  // namespace pq::app
