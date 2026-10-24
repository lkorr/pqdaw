#pragma once

// What each CLI mode does. Return values are process exit codes (0 ok, 1 failure, kExitSkipped).

#include "pq/app/cli.h"

namespace pq::app {

int runScreenshot(const CliOptions& options);
// Interactive window (selfTest = false) or the scripted resize/minimise/restore check (selfTest = true).
int runWindow(const CliOptions& options, bool selfTest);
// --list-devices: prints every device of each compiled audio backend (audio_commands.cpp).
int runListDevices(const CliOptions& options);
// --sine: plays a test tone through the device + BlockAdapter path and prints callback stats; exit 3 on xruns
// (with --inject-stall: exit 0 iff exactly one underrun was reported).
int runSine(const CliOptions& options);
// --latency-test: shared-mode WASAPI output latency, impulses vs a loopback capture of the endpoint; exit 3 if the
// reported latency is off by more than one period.
int runLatencyTest(const CliOptions& options);
// --asio-panel (T0.6-fix2): loads an ASIO driver (a stream is opened, not started), opens its control panel and
// keeps the driver loaded until Enter (or --seconds); manual check only.
int runAsioPanel(const CliOptions& options);
// --play (T1.7, play_command.cpp): compile a .pqp patch with the interpreter and play its global outputs.
int runPlay(const CliOptions& options);
// --render (T3.17, render_command.cpp): a project rendered offline through the playback engine into a WAV file.
int runRender(const CliOptions& options);

}  // namespace pq::app
