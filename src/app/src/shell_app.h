#pragma once

// The DAW shell in the window (T3.7): owns the ViewContext (project), the app's TransportPort (SimulatedTransport
// until T3.17), the SDL file dialogs, and the Shell built into the window loop's UiTree. The window loop
// (commands.cpp) treats the shell like any widget tree and calls three hooks:
//   handleEvent(e, consumed)  after the tree saw an SDL event: dialog results, quit confirmation, SDL text input on/off
//                             with text focus; returns true when the loop must not act on the event itself
//   onFramePresented()        after each presented frame: first-frame timing log, the --idle-check measurement, the
//                             autosave policy (AS.1: pq/app/autosave.h; a one-shot timer wakes it when no frame comes)
//   exitCode(rc)              the process exit code (the idle check's verdict)

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

#include "pq/app/autosave.h"
#include "pq/app/bug_reporter.h"
#include "pq/app/face_voices.h"
#include "pq/app/cli.h"
#include "pq/app/os_drop.h"
#include "pq/app/patch_preview.h"
#include "pq/app/preview_service.h"
#include "pq/library/indexer.h"
#include "pq/app/project_link.h"
#include "pq/app/sim_transport.h"
#include "pq/app/startup.h"
#include "pq/core/result.h"
#include "pq/ui/widget.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {
class DeviceChainView;
}
namespace pq::views::project_patch {
class ProjectPatchView;
}

namespace pq::app {

class SdlFileDialogs;
class AudioDeviceManager;
class MidiInputService;
class ExportService;
class RenderEditService;

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

  // T5.x-bugreport: the F12 bug report (pq/app/bug_reporter.h); the window loop calls it for F12 and records every
  // input event the tree dispatched into it.
  BugReporter* bugReporter() noexcept { return bug_.get(); }
  void recordInput(const ui::InputEvent& e) noexcept {
    if (bug_) bug_->recordInput(e);
  }

private:
  ShellApp(ui::UiTree& tree, SDL_Window* window);
  void updateTitle();
  void syncTextInput();
  void onTempoMaybeChanged();
  void syncPreviewRate();
  // T5.7: files dragged in from Explorer (pq/app/os_drop.h): queued from the OLE callback, played into the tree here.
  ui::DropEffect onOsDrag(const OsDragEvent& e);
  void drainOsDrags();
  // T5.12: an internal drag left the window: files (browser items) or clips (rendered to WAV) go out as an OLE drag.
  void onDragOut(ui::DragPayload p);

  ui::UiTree& tree_;
  SDL_Window* window_;
  // T3.17: the transport is the playback link (the engine); the simulated one only if the link cannot start.
  // Declared before ctx_ (the context holds a reference to the port); audio_ after link_ (it calls the engine).
  std::unique_ptr<SimulatedTransport> transport_;
  std::unique_ptr<ProjectLink> link_;
  // TMM.5: the meta faces' voice readers over the link's taps (after link_; before the views that bind them).
  std::unique_ptr<FaceVoiceHub> faceVoices_;
  views::project_patch::ProjectPatchView* faceView_ = nullptr;  // the view whose faces it feeds (owned by shell_)
  views::DeviceChainView* faceChain_ = nullptr;  // TMM.10: the Device view (chain meta devices' faces; owned by shell_)
  // T4.4: the circuit editor's patch preview, added into the device after the engine (declared before audio_: the
  // output stops calling it first).
  std::unique_ptr<PatchPreview> preview_;
  engine::AudioCallback previewCallback_{};
  std::unique_ptr<AudioDeviceManager> audio_;  // T3.18: the device manager (owns the audio output; Ctrl+, prefs)
  std::unique_ptr<MidiInputService> midi_;     // T6.4: MIDI input ports (after audio_: closed first)
  std::unique_ptr<PreviewService> browserPreview_;  // T5.10: the browser's preview (the output's cue source)
  std::unique_ptr<library::Indexer> indexer_;      // T5.10: the browser index of the places
  std::unique_ptr<views::ViewContext> ctx_;
  std::unique_ptr<ExportService> export_;  // EX.2: the Export Audio job (its worker); destroyed before the context
  std::unique_ptr<RenderEditService> renderEdits_;  // FZ.4: flatten / bounce / consolidate renders (its worker)
  std::unique_ptr<SdlFileDialogs> dialogs_;
  std::unique_ptr<views::Shell> shell_;
  std::unique_ptr<BugReporter> bug_;  // T5.x-bugreport (after shell_: destroyed first)
  // PQ_BUGREPORT_SCRIPT: the steps still to run (bug_reporter.h), one per timer tick.
  std::vector<std::string> bugScript_;
  void runBugScriptStep();
  views::ViewContext::Subscription titleSub_, tempoSub_;
  const void* tempoTimeline_ = nullptr;
  bool textInput_ = false;
  uint32_t userEvent_ = 0;  // SDL user event type for dialog results and idle-check timers
  std::unique_ptr<OsDropTarget> drop_;  // T5.7
  std::vector<OsDragEvent> dropQueue_;
  ui::DropEffect dropEffect_ = ui::DropEffect::None;  // what the tree said last (the OLE callback's answer)
  bool dropPosted_ = false;
  uint64_t quitAskedAt_ = 0;  // SDL ticks (ns) when the user was last asked about unsaved changes on quit
  bool quitAnswer_ = true;
  bool quitConfirmed_ = false;  // AS.1: confirmQuit let a quit go ahead (a clean quit deletes the autosave)
  // AS.1: the autosave of this run's session folder (null without the playback link: no session folder).
  std::unique_ptr<Autosaver> autosave_;
  bool autosaveTimer_ = false;  // its wake timer is armed
  void pollAutosave();

  // First frame + --idle-check.
  uint32_t frames_ = 0;
  double idleSeconds_ = 0;
  uint32_t framesAtStart_ = 0;
  uint64_t events_ = 0, eventsAtStart_ = 0;
  double cpuAtStart_ = 0, wallAtStart_ = 0;
  uint64_t cyclesAtStart_ = 0, tscAtStart_ = 0, uiCyclesAtStart_ = 0;
  std::vector<ThreadCycles> threadsAtStart_;
  int idleVerdict_ = -1;
  // T0.x-flake: the measurement starts only after 1 s without frames or events (settle), and a window that SDL
  // events reached (another program's windows taking focus or covering ours) is measured again.
  uint32_t settleFrames_ = 0;
  uint64_t settleEvents_ = 0;
  int settleTries_ = 0, idleAttempts_ = 0;
  friend class SdlFileDialogs;
};

// Startup log line printed once the first frame is presented (also used by the idle check).
std::string firstFrameReport();

}  // namespace pq::app
