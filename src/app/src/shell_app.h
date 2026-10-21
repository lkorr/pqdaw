#pragma once

// The DAW shell in the window (T3.7): owns the ViewContext (project), the app's TransportPort (SimulatedTransport
// until T3.17), the SDL file dialogs, and the Shell built into the window loop's UiTree. The window loop
// (commands.cpp) treats the shell like any widget tree and calls three hooks:
//   handleEvent(e, consumed)  after the tree saw an SDL event: dialog results, quit confirmation, SDL text input on/off
//                             with text focus; returns true when the loop must not act on the event itself
//   onFramePresented()        after each presented frame: first-frame timing log, the --idle-check measurement
//   exitCode(rc)              the process exit code (the idle check's verdict)

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

#include "pq/app/cli.h"
#include "pq/app/sim_transport.h"
#include "pq/app/startup.h"
#include "pq/core/result.h"
#include "pq/ui/widget.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::app {

class SdlFileDialogs;

class ShellApp {
public:
  // Builds the shell into `tree` (viewport and resources already set) and loads options.project (if any).
  static Result<std::unique_ptr<ShellApp>> create(ui::UiTree& tree, SDL_Window* window, const CliOptions& options);
  ~ShellApp();
  ShellApp(const ShellApp&) = delete;
  ShellApp& operator=(const ShellApp&) = delete;

  bool handleEvent(const SDL_Event& e, bool consumedByTree);
  void onFramePresented();
  int exitCode(int loopResult) const noexcept { return idleVerdict_ >= 0 ? idleVerdict_ : loopResult; }

  views::ViewContext& context() noexcept { return *ctx_; }

private:
  ShellApp(ui::UiTree& tree, SDL_Window* window);
  void updateTitle();
  void syncTextInput();
  void onTempoMaybeChanged();

  ui::UiTree& tree_;
  SDL_Window* window_;
  std::unique_ptr<SimulatedTransport> transport_;
  std::unique_ptr<views::ViewContext> ctx_;
  std::unique_ptr<SdlFileDialogs> dialogs_;
  std::unique_ptr<views::Shell> shell_;
  views::ViewContext::Subscription titleSub_, tempoSub_;
  const void* tempoTimeline_ = nullptr;
  bool textInput_ = false;
  uint32_t userEvent_ = 0;  // SDL user event type for dialog results and idle-check timers
  uint64_t quitAskedAt_ = 0;  // SDL ticks (ns) when the user was last asked about unsaved changes on quit
  bool quitAnswer_ = true;

  // First frame + --idle-check.
  uint32_t frames_ = 0;
  double idleSeconds_ = 0;
  uint32_t framesAtStart_ = 0;
  uint64_t events_ = 0, eventsAtStart_ = 0;
  double cpuAtStart_ = 0, wallAtStart_ = 0;
  uint64_t cyclesAtStart_ = 0, tscAtStart_ = 0, uiCyclesAtStart_ = 0;
  std::vector<ThreadCycles> threadsAtStart_;
  int idleVerdict_ = -1;
  friend class SdlFileDialogs;
};

// Startup log line printed once the first frame is presented (also used by the idle check).
std::string firstFrameReport();

}  // namespace pq::app
