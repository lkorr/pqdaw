#pragma once

// What each CLI mode does. Return values are process exit codes (0 ok, 1 failure, kExitSkipped).

#include "pq/app/cli.h"

namespace pq::app {

int runScreenshot(const CliOptions& options);
// Interactive window (selfTest = false) or the scripted resize/minimise/restore check (selfTest = true).
int runWindow(const CliOptions& options, bool selfTest);
// --list-devices: prints every device of each compiled audio backend (audio_commands.cpp).
int runListDevices(const CliOptions& options);
// --sine: plays a test tone through the device + BlockAdapter path and prints callback stats; exit 3 on xruns.
int runSine(const CliOptions& options);

}  // namespace pq::app
